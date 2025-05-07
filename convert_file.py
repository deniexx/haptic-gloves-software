import librosa
import numpy as np
import json
import os

# --- Configuration ---
DEFAULT_HAPTIC_DURATION = 0.15  # seconds
MIN_HAPTIC_STRENGTH = 50       # Minimum strength for *any* haptic event (0-255)
MAX_HAPTIC_STRENGTH = 255      # Maximum strength for a haptic event (0-255)
RMS_FILTER_THRESHOLD = 0.05    # Relative RMS threshold (0.0 to 1.0) to filter very quiet segments *before* FFT

# --- Frequency band definitions (5 Bands) ---
# Adjusted ranges to potentially increase Pinky activity
FREQ_BANDS = {
    # band_name: {"range": (min_hz, max_hz), "finger_id": finger_index}
    "sub_bass": {"range": (20, 100), "finger_id": 0},    # Thumb
    "low_mid":  {"range": (101, 400), "finger_id": 1},   # Index
    "mid":      {"range": (401, 1500), "finger_id": 2},  # Middle
    "upper_mid":{"range": (1501, 4000), "finger_id": 3}, # Ring (End lowered)
    "high":     {"range": (4001, 12000), "finger_id": 4} # Pinky (Start lowered)
}
# Minimum energy in a band (relative to max energy in *that specific segment*) to trigger its finger
BAND_ENERGY_THRESHOLD_FACTOR = 0.20 # Adjust as needed (lower means more fingers trigger easily)

# --- Advanced Configuration ---
N_FFT = 2048            # FFT window size
HOP_LENGTH_FFT = 512    # Hop length for FFT analysis
HOP_LENGTH_ONSET = 512  # Hop length for onset detection
ONSET_WAIT_TIME = 0.03  # seconds - minimum time between consecutive onsets to process
SEGMENT_PRE_ONSET = 0.05 # seconds - how much time before onset to include in analysis segment
SEGMENT_POST_ONSET_FACTOR = 1.0 # Multiplier for DEFAULT_HAPTIC_DURATION to determine analysis segment end

# --- Helper Functions ---

def analyze_and_generate_events(audio_path):
    """
    Analyzes the audio file using onsets and frequency bands
    to generate a list of haptic events.
    """
    print(f"Loading audio file: {audio_path}...")
    try:
        y, sr = librosa.load(audio_path, sr=None, mono=False)
    except Exception as e:
        print(f"Error loading audio file '{os.path.basename(audio_path)}': {e}")
        return None

    events = []
    is_stereo = y.ndim > 1 and y.shape[0] == 2

    if is_stereo:
        print(f"  Stereo audio detected for '{os.path.basename(audio_path)}'. Processing left and right channels.")
        y_left = y[0]
        y_right = y[1]
        process_channel(y_left, sr, 0, events, os.path.basename(audio_path), channel_name="Left")
        process_channel(y_right, sr, 1, events, os.path.basename(audio_path), channel_name="Right")
    else:
        print(f"  Mono audio detected for '{os.path.basename(audio_path)}'. Distributing to hands based on analysis.")
        y_mono = y
        # Process mono for both hands, frequency analysis determines fingers
        process_channel(y_mono, sr, 0, events, os.path.basename(audio_path), channel_name="Mono (as Left)")
        process_channel(y_mono, sr, 1, events, os.path.basename(audio_path), channel_name="Mono (as Right)")

    events.sort(key=lambda x: x["timestamp"])
    print(f"  Generated {len(events)} haptic events for '{os.path.basename(audio_path)}'.")
    return events

def process_channel(y_channel, sr, hand_id, events_list, filename, channel_name=None):
    """
    Processes a single audio channel using onset detection and frequency analysis.
    """
    target_name = f"channel for hand_id {hand_id}"
    if channel_name:
        target_name = f"{channel_name} channel for hand_id {hand_id}"
    print(f"    Processing {target_name} of '{filename}'...")

    # 1. Onset Detection
    print("      Detecting onsets...")
    onset_frames = librosa.onset.onset_detect(y=y_channel, sr=sr, hop_length=HOP_LENGTH_ONSET, units='frames', wait=int(ONSET_WAIT_TIME*sr/HOP_LENGTH_ONSET))
    onset_times = librosa.frames_to_time(onset_frames, sr=sr, hop_length=HOP_LENGTH_ONSET)
    print(f"        Detected {len(onset_times)} onsets.")

    if len(onset_times) == 0:
        print("        No onsets detected for this channel.")
        return

    # 2. Overall RMS for dynamic range normalization
    frame_length_rms = 2048
    hop_length_rms = 512
    rms_overall_channel = librosa.feature.rms(y=y_channel, frame_length=frame_length_rms, hop_length=hop_length_rms)[0]
    min_rms_song = np.min(rms_overall_channel) if len(rms_overall_channel) > 0 else 0.0
    max_rms_song = np.max(rms_overall_channel) if len(rms_overall_channel) > 0 else 0.1
    dynamic_range = max_rms_song - min_rms_song
    if dynamic_range < 1e-5: 
        dynamic_range = 0.1
        min_rms_song = 0.0 
        max_rms_song = 0.1

    # 3. Generate haptic events based on onsets and frequency analysis
    print("      Generating haptic events...")
    
    for onset_time in onset_times:
        # Define segment for analysis around the onset
        segment_start_time = onset_time - SEGMENT_PRE_ONSET
        segment_end_time = onset_time + (DEFAULT_HAPTIC_DURATION * SEGMENT_POST_ONSET_FACTOR)
        
        start_sample = librosa.time_to_samples(segment_start_time, sr=sr)
        end_sample = librosa.time_to_samples(segment_end_time, sr=sr)
        start_sample = max(0, start_sample)
        end_sample = min(len(y_channel), end_sample)

        if start_sample >= end_sample: continue
        onset_segment = y_channel[start_sample:end_sample]
        if len(onset_segment) == 0: continue

        # Calculate overall RMS of this segment
        segment_rms_value = np.sqrt(np.mean(onset_segment**2))
        normalized_segment_rms = (segment_rms_value - min_rms_song) / dynamic_range if dynamic_range > 1e-5 else 0.5
        
        # --- Filter quiet segments early ---
        if normalized_segment_rms < RMS_FILTER_THRESHOLD : 
            # print(f"          Skipping quiet segment at {onset_time:.2f}s (RMS below threshold)")
            continue
            
        # Perform STFT for frequency analysis
        S_segment = librosa.stft(onset_segment, n_fft=N_FFT, hop_length=HOP_LENGTH_FFT)
        S_mag_segment = np.abs(S_segment)
        freqs = librosa.fft_frequencies(sr=sr, n_fft=N_FFT)

        band_energies = {}
        max_energy_in_segment = 0.0

        # Calculate energy in each defined band for this segment
        for band_name, band_info in FREQ_BANDS.items():
            min_f, max_f = band_info["range"]
            idx = np.where((freqs >= min_f) & (freqs <= max_f))[0]
            if len(idx) > 0:
                # Use mean of magnitudes in the band as its energy representation
                energy = np.mean(S_mag_segment[idx, :]) 
                band_energies[band_name] = energy
                if energy > max_energy_in_segment:
                    max_energy_in_segment = energy
            else:
                band_energies[band_name] = 0.0
        
        # Avoid processing essentially silent segments after FFT
        if max_energy_in_segment < 1e-6: continue

        # Generate events for bands that meet the energy threshold
        for band_name, energy in band_energies.items():
            if energy >= max_energy_in_segment * BAND_ENERGY_THRESHOLD_FACTOR and energy > 1e-5: # Add small absolute floor
                finger_id = FREQ_BANDS[band_name]["finger_id"]
                
                # --- Calculate strength based on band energy and overall segment RMS ---
                band_strength_factor = (energy / max_energy_in_segment) if max_energy_in_segment > 1e-6 else 0.0
                effective_strength_normalized = normalized_segment_rms * band_strength_factor
                specific_strength = int(MIN_HAPTIC_STRENGTH + effective_strength_normalized * (MAX_HAPTIC_STRENGTH - MIN_HAPTIC_STRENGTH))
                specific_strength = max(MIN_HAPTIC_STRENGTH, min(MAX_HAPTIC_STRENGTH, specific_strength))

                event = {
                    "timestamp": round(onset_time, 3),
                    "hand_id": hand_id,
                    "finger_id": finger_id,
                    "strength": specific_strength,
                    "duration": DEFAULT_HAPTIC_DURATION
                }
                events_list.append(event)
                # print(f"          Event: t={onset_time:.2f}s, hand={hand_id}, finger={finger_id} ({band_name}), str={specific_strength} (NRMS:{normalized_segment_rms:.2f}, BSF:{band_strength_factor:.2f})")


def save_haptic_file(events, output_path):
    """ Saves the list of haptic events to a JSON file. """
    print(f"    Saving haptic data to: {output_path}...")
    try:
        with open(output_path, 'w') as f:
            json.dump(events, f, indent=4)
    except Exception as e:
        print(f"    Error saving haptic file '{os.path.basename(output_path)}': {e}")

# --- Main Execution ---
if __name__ == "__main__":
    songs_input_folder_name = "songs"
    haptic_output_folder_name = "haptic_outputs"
    supported_extensions = ('.wav', '.mp3', '.flac', '.ogg', '.m4a', '.aac')

    script_dir = os.path.dirname(os.path.abspath(__file__)) if "__file__" in locals() else os.getcwd()
    songs_folder_path = os.path.join(script_dir, songs_input_folder_name)
    output_folder_path = os.path.join(script_dir, haptic_output_folder_name)

    if not os.path.isdir(songs_folder_path):
        print(f"Error: Input songs folder not found at '{songs_folder_path}'")
        print(f"Please create a folder named '{songs_input_folder_name}' in the script's directory and place your songs there.")
    else:
        if not os.path.exists(output_folder_path):
            print(f"Creating output directory: '{output_folder_path}'")
            os.makedirs(output_folder_path)

        print(f"Starting batch haptic conversion from folder: '{songs_folder_path}'")
        print(f"Output will be saved to: '{output_folder_path}'")
        
        processed_files_count = 0
        found_audio_files_count = 0

        for filename in os.listdir(songs_folder_path):
            if filename.lower().endswith(supported_extensions):
                found_audio_files_count += 1
                print(f"\nProcessing file: {filename}...")
                song_file_path = os.path.join(songs_folder_path, filename)
                
                haptic_events_list = analyze_and_generate_events(song_file_path)

                if haptic_events_list is not None and len(haptic_events_list) > 0 : 
                    base_name_no_ext = os.path.splitext(filename)[0]
                    output_json_filename = f"{base_name_no_ext}_haptics.json"
                    output_json_path = os.path.join(output_folder_path, output_json_filename)
                    
                    save_haptic_file(haptic_events_list, output_json_path)
                    processed_files_count +=1
                elif haptic_events_list is not None and len(haptic_events_list) == 0:
                     print(f"  No haptic events generated for '{filename}' (e.g. too quiet or no distinct onsets/features found).")

        if found_audio_files_count == 0:
            print(f"\nNo audio files found in '{songs_folder_path}'. Make sure files have supported extensions: {supported_extensions}")
        else:
            print(f"\nBatch processing complete. Successfully generated haptic files for {processed_files_count}/{found_audio_files_count} audio files.")
            print(f"Haptic JSON files saved in '{output_folder_path}'.")