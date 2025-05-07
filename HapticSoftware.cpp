#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem> // C++17
#include <iomanip> 

#define STB_IMAGE_IMPLEMENTATION
#define _CRT_SECURE_NO_WARNINGS

// Define MINIAUDIO_IMPLEMENTATION in exactly one .c or .cpp file
#define MINIAUDIO_IMPLEMENTATION
#include "vendor/miniaudio.h" // Ensure this path is correct

#include "vendor/imgui/backends/imgui_impl_dx11.h"
#include "vendor/imgui/backends/imgui_impl_win32.h"
#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "vendor/seriallib/serialib.h"
#include "vendor/stb_image.h"
#include "vendor/json.hpp" // JSON parsing

#include <cstdint>
#include <d3d11.h>
#include <tchar.h>

// Namespace aliases
namespace fs = std::filesystem;
using json = nlohmann::json;

enum class ETargetHandLocation : uint8_t 
{
  Thumb = 0, Index = 1, Middle = 2, Ring = 3, Pinky = 4, None = 5
};

struct FingerConfig 
{
  ETargetHandLocation Location = ETargetHandLocation::None;
  int Strength = 0;
  float Duration = 0.f; 
  double LastWriteTime = 0.0;
};

struct HapticEvent {
    double timestamp = 0.0;
    int hand_id = 0;
    uint8_t finger_id = 0;
    uint8_t strength = 0;
    float duration = 0.f;
    bool operator<(const HapticEvent& other) const {
        return timestamp < other.timestamp;
    }
};

// --- Global D3D variables ---
static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

// --- Application state variables ---
static double g_timeSinceStart = 0.0; 

// --- Manual Control Configuration ---
static std::string StrengthTitle = "Strength";
static std::string DurationTitle = "Duration (Manual)";
static bool immediateMode = true; 
constexpr float g_immediateModeDuration = 0.2f; 
static uint8_t paddingBuffer[2] = {0, 0}; 

// --- UI Configuration ---
constexpr float imagePadding = 25.f;

// --- Finger Configurations ---
static std::vector<FingerConfig> g_leftHandFingers;
static std::vector<FingerConfig> g_rightHandFingers;
const int NUM_FINGERS_PER_HAND = 5; 

// --- Haptic Song Playback Globals ---
static std::vector<HapticEvent> g_scheduled_events;
static std::vector<std::string> g_available_haptic_files; // .json files
static int g_current_selected_haptic_file_index = -1;
static std::string g_haptic_files_directory = "haptic_outputs"; 
static std::string g_audio_files_directory = "songs"; // Directory for audio files
static bool g_playback_active = false;
static double g_playback_start_time_global = 0.0; 
static int g_next_event_index = 0;
static std::string g_currently_playing_file = ""; // Name of the haptic .json file
static char g_haptic_file_load_error[256] = ""; 
static char g_audio_file_load_error[256] = ""; // For audio loading errors

// --- Audio Playback Globals (miniaudio) ---
static ma_engine g_audio_engine;
static ma_sound g_current_song_sound; // Sound object for the current song
static bool g_is_current_song_sound_initialized = false; // To track if g_current_song_sound is valid


// --- Forward Declarations ---
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool LoadTextureFromFile(const char *file_name, ID3D11ShaderResourceView **out_srv, int *out_width, int *out_height);
bool LoadTextureFromMemory(const void *data, size_t data_size, ID3D11ShaderResourceView **out_srv, int *out_width, int *out_height);
std::string GetFingerText(ETargetHandLocation Location);
ImVec4 LerpColorHSV(const ImVec4& srgbColor1, const ImVec4& srgbColor2, float t);
void RefreshHapticFileList();
bool LoadHapticEvents(const std::string& haptic_filename_without_path);
bool LoadAndPrepareAudio(const std::string& haptic_filename_without_path); 
void StopAndUnloadAudio(); 


// Main code
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) 
{
  WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"Haptic Software", nullptr};
  ::RegisterClassExW(&wc);
  HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Haptic Software", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

  if (!CreateDeviceD3D(hwnd)) {
    CleanupDeviceD3D();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  ::ShowWindow(hwnd, SW_SHOWDEFAULT);
  ::UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; 
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; 

  ma_result audio_result;
  audio_result = ma_engine_init(NULL, &g_audio_engine);
  if (audio_result != MA_SUCCESS) {
      ImGui::DebugLog("Failed to initialize audio engine: %s\n", ma_result_description(audio_result));
  } else {
      ImGui::DebugLog("Audio engine initialized successfully.\n");
  }


  ImGui::StyleColorsDark();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  ImFont* titleFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 24);
  ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 20);
  ImFont* smallFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 16);
  io.Fonts->Build();
  io.FontDefault = smallFont;

  ImVec4 clear_color = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
  ImVec4 initialColor = ImVec4(0.2f, 0.4f, 0.92f, 1.f);
  ImVec4 targetColor = ImVec4(1.f, 0.f, 0.f, 1.f);
  ImVec4 transparent = ImVec4(0.f, 0.f, 0.f, 0.f);

  serialib leftHand;
  serialib rightHand;

  if (leftHand.openDevice("\\\\.\\COM6", 9600) != 1) { ImGui::DebugLog("Failed to open COM6 for Left Hand.\n"); }
  if (rightHand.openDevice("\\\\.\\COM11", 9600) != 1) { ImGui::DebugLog("Failed to open COM11 for Right Hand.\n"); }

  g_leftHandFingers.resize(NUM_FINGERS_PER_HAND);
  g_rightHandFingers.resize(NUM_FINGERS_PER_HAND);
  ETargetHandLocation fingerLocations[] = {
      ETargetHandLocation::Thumb, ETargetHandLocation::Index, ETargetHandLocation::Middle,
      ETargetHandLocation::Ring, ETargetHandLocation::Pinky
  };
  for (int i = 0; i < NUM_FINGERS_PER_HAND; ++i) {
      g_leftHandFingers[i] = {fingerLocations[i], 0, g_immediateModeDuration, 0.0};
      g_rightHandFingers[i] = {fingerLocations[i], 0, g_immediateModeDuration, 0.0};
  }

  float imageDownScale = 3.75f;
  int imageWidth = 0, imageHeight = 0;
  ID3D11ShaderResourceView* handTexture = nullptr, *indexTexture = nullptr, *middleTexture = nullptr, *ringTexture = nullptr, *pinkyTexture = nullptr, *thumbTexture = nullptr;
  LoadTextureFromFile(".\\Assets\\hand.png", &handTexture, &imageWidth, &imageHeight);
  LoadTextureFromFile(".\\Assets\\index.png", &indexTexture, &imageWidth, &imageHeight);
  LoadTextureFromFile(".\\Assets\\middle.png", &middleTexture, &imageWidth, &imageHeight);
  LoadTextureFromFile(".\\Assets\\ring.png", &ringTexture, &imageWidth, &imageHeight);
  LoadTextureFromFile(".\\Assets\\pinky.png", &pinkyTexture, &imageWidth, &imageHeight);
  LoadTextureFromFile(".\\Assets\\thumb.png", &thumbTexture, &imageWidth, &imageHeight);

  ImVec2 normalUv0(0.f, 0.f), normalUv1(1.f, 1.f), flipUv0(1.f, 0.f), flipUv1(0.f, 1.f);
  float drawImageWidth = imageWidth / imageDownScale;
  float drawImageHeight = imageHeight / imageDownScale;

  ImGuiStyle * style = &ImGui::GetStyle();
  style->WindowPadding = ImVec2(15, 15); style->WindowRounding = 5.0f; style->FramePadding = ImVec2(5, 5); style->FrameRounding = 4.0f; style->ItemSpacing = ImVec2(12, 8); style->ItemInnerSpacing = ImVec2(8, 6); style->IndentSpacing = 25.0f; style->ScrollbarSize = 15.0f; style->ScrollbarRounding = 9.0f; style->GrabMinSize = 5.0f; style->GrabRounding = 3.0f;
  style->Colors[ImGuiCol_Text]                  = ImVec4(0.80f, 0.80f, 0.83f, 1.00f); style->Colors[ImGuiCol_TextDisabled]          = ImVec4(0.24f, 0.23f, 0.29f, 1.00f); style->Colors[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.05f, 0.07f, 1.00f); style->Colors[ImGuiCol_PopupBg]               = ImVec4(0.07f, 0.07f, 0.09f, 1.00f); style->Colors[ImGuiCol_Border]                = ImVec4(0.80f, 0.80f, 0.83f, 0.88f); style->Colors[ImGuiCol_BorderShadow]          = ImVec4(0.92f, 0.91f, 0.88f, 0.00f); style->Colors[ImGuiCol_FrameBg]               = ImVec4(0.10f, 0.09f, 0.12f, 1.00f); style->Colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.24f, 0.23f, 0.29f, 1.00f); style->Colors[ImGuiCol_FrameBgActive]         = ImVec4(0.56f, 0.56f, 0.58f, 1.00f); style->Colors[ImGuiCol_TitleBg]               = ImVec4(0.10f, 0.09f, 0.12f, 1.00f); style->Colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(1.00f, 0.98f, 0.95f, 0.75f); style->Colors[ImGuiCol_TitleBgActive]         = ImVec4(0.07f, 0.07f, 0.09f, 1.00f); style->Colors[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.09f, 0.12f, 1.00f); style->Colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.10f, 0.09f, 0.12f, 1.00f); style->Colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.80f, 0.80f, 0.83f, 0.31f); style->Colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.56f, 0.56f, 0.58f, 1.00f); style->Colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.06f, 0.05f, 0.07f, 1.00f); style->Colors[ImGuiCol_CheckMark]             = ImVec4(0.80f, 0.80f, 0.83f, 0.31f); style->Colors[ImGuiCol_SliderGrab]            = ImVec4(0.80f, 0.80f, 0.83f, 0.31f); style->Colors[ImGuiCol_SliderGrabActive]        = ImVec4(0.06f, 0.05f, 0.07f, 1.00f); style->Colors[ImGuiCol_Button]                = ImVec4(0.10f, 0.09f, 0.12f, 1.00f); style->Colors[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.23f, 0.29f, 1.00f); style->Colors[ImGuiCol_ButtonActive]          = ImVec4(0.56f, 0.56f, 0.58f, 1.00f); style->Colors[ImGuiCol_Header]                = ImVec4(0.10f, 0.09f, 0.12f, 1.00f); style->Colors[ImGuiCol_HeaderHovered]         = ImVec4(0.56f, 0.56f, 0.58f, 1.00f); style->Colors[ImGuiCol_HeaderActive]          = ImVec4(0.06f, 0.05f, 0.07f, 1.00f); style->Colors[ImGuiCol_ResizeGrip]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f); style->Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.56f, 0.56f, 0.58f, 1.00f); style->Colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.06f, 0.05f, 0.07f, 1.00f); style->Colors[ImGuiCol_PlotLines]             = ImVec4(0.40f, 0.39f, 0.38f, 0.63f); style->Colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.25f, 1.00f, 0.00f, 1.00f); style->Colors[ImGuiCol_PlotHistogram]         = ImVec4(0.40f, 0.39f, 0.38f, 0.63f); style->Colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.25f, 1.00f, 0.00f, 1.00f); style->Colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);

  RefreshHapticFileList();

  bool done = false;
  while (!done) 
  {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      ::TranslateMessage(&msg); ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;

    if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) { ::Sleep(10); continue; }
    g_SwapChainOccluded = false;

    if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
      CleanupRenderTarget();
      g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
      g_ResizeWidth = g_ResizeHeight = 0;
      CreateRenderTarget();
    }

    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    g_timeSinceStart += io.DeltaTime; 

    ImVec2 screenSize = io.DisplaySize;
    
    if (g_playback_active) { ImGui::BeginDisabled(); }
    // Left Hand Manual Control Window
    {
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x * 0.2f, screenSize.y * 0.5f));
      ImGui::Begin("Left Hand", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
      if (leftHand.isDeviceOpen()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.f); ImGui::PushFont(titleFont);
        ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Left Hand").x) * 0.5f);
        ImGui::Text("Left Hand"); ImGui::PopFont(); ImGui::Separator();
        for (int i = 0; i < NUM_FINGERS_PER_HAND; ++i) { 
          ImGui::PushID(i); std::string fingerName = GetFingerText(g_leftHandFingers[i].Location);
          ImGui::Text("%s", fingerName.c_str()); ImGui::SameLine(100); 
          ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
          ImGui::DragInt(StrengthTitle.c_str(), &g_leftHandFingers[i].Strength, 1.f, 0, 255);
          if (!immediateMode) { ImGui::DragFloat(DurationTitle.c_str(), &g_leftHandFingers[i].Duration, 0.01f, 0.05f, 10.f, "%.2f s");}
          ImGui::PopItemWidth(); ImGui::PopID();
        }
      } else { ImGui::TextColored(ImVec4(1.f,0.f,0.f,1.f), "Left Hand: COM6 Not Open"); }
      ImGui::End();
    }
    // Right Hand Manual Control Window
    {
      ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.8f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x * 0.2f, screenSize.y * 0.5f));
      ImGui::Begin("Right Hand", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
       if (rightHand.isDeviceOpen()){
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.f); ImGui::PushFont(titleFont);
        ImGui::SetCursorPosX((ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Right Hand").x) * 0.5f);
        ImGui::Text("Right Hand"); ImGui::PopFont(); ImGui::Separator();
        for (int i = 0; i < NUM_FINGERS_PER_HAND; ++i) { 
            ImGui::PushID(NUM_FINGERS_PER_HAND + i); 
            std::string fingerName = GetFingerText(g_rightHandFingers[i].Location);
            ImGui::Text("%s", fingerName.c_str()); ImGui::SameLine(100);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
            ImGui::DragInt(StrengthTitle.c_str(), &g_rightHandFingers[i].Strength, 1.f, 0, 255);
            if (!immediateMode) { ImGui::DragFloat(DurationTitle.c_str(), &g_rightHandFingers[i].Duration, 0.01f, 0.05f, 10.f, "%.2f s");}
            ImGui::PopItemWidth(); ImGui::PopID();
        }
       } else { ImGui::TextColored(ImVec4(1.f,0.f,0.f,1.f), "Right Hand: COM11 Not Open");}
      ImGui::End();
    }
    if (g_playback_active) { ImGui::EndDisabled(); }

    // Hand Image Display Window
    {
      ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.2f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x * 0.6f, screenSize.y * 0.5f));
      ImGui::Begin("Hand Visualization", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
      ImVec2 panelSize = ImGui::GetContentRegionAvail();
      float totalHandsWidth = (drawImageWidth * 2) + imagePadding * 3; 
      float startX = (panelSize.x - totalHandsWidth) * 0.5f;
      if (startX < 0) startX = imagePadding;
      ImVec2 imagePosLeft = ImVec2(startX, panelSize.y * 0.5f - drawImageHeight * 0.5f);
      ImVec2 imagePosRight = ImVec2(startX + drawImageWidth + imagePadding, panelSize.y * 0.5f - drawImageHeight * 0.5f);

      ImGui::SetCursorPos(imagePosLeft); ImGui::Image((ImTextureID)handTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, initialColor, transparent);
      ImGui::SetCursorPos(imagePosLeft); ImGui::Image((ImTextureID)indexTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, LerpColorHSV(initialColor, targetColor, g_leftHandFingers[1].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosLeft); ImGui::Image((ImTextureID)middleTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, LerpColorHSV(initialColor, targetColor, g_leftHandFingers[2].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosLeft); ImGui::Image((ImTextureID)ringTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, LerpColorHSV(initialColor, targetColor, g_leftHandFingers[3].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosLeft); ImGui::Image((ImTextureID)pinkyTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, LerpColorHSV(initialColor, targetColor, g_leftHandFingers[4].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosLeft); ImGui::Image((ImTextureID)thumbTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, LerpColorHSV(initialColor, targetColor, g_leftHandFingers[0].Strength / 255.f), transparent);

      ImGui::SetCursorPos(imagePosRight); ImGui::Image((ImTextureID)handTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, initialColor, transparent);
      ImGui::SetCursorPos(imagePosRight); ImGui::Image((ImTextureID)indexTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, LerpColorHSV(initialColor, targetColor, g_rightHandFingers[1].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosRight); ImGui::Image((ImTextureID)middleTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, LerpColorHSV(initialColor, targetColor, g_rightHandFingers[2].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosRight); ImGui::Image((ImTextureID)ringTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, LerpColorHSV(initialColor, targetColor, g_rightHandFingers[3].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosRight); ImGui::Image((ImTextureID)pinkyTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, LerpColorHSV(initialColor, targetColor, g_rightHandFingers[4].Strength / 255.f), transparent);
      ImGui::SetCursorPos(imagePosRight); ImGui::Image((ImTextureID)thumbTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, LerpColorHSV(initialColor, targetColor, g_rightHandFingers[0].Strength / 255.f), transparent);
      ImGui::End();
    }

    // Haptic Song Player Window
    {
      ImGui::SetNextWindowPos(ImVec2(0.f, screenSize.y * 0.5f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x, screenSize.y * 0.2f));
      ImGui::Begin("Haptic Song Player", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove );
      
      ImGui::PushFont(titleFont); ImGui::Text("Haptic Song Player"); ImGui::PopFont(); ImGui::Separator();

      if (ImGui::Button("Refresh Haptic Files")) { RefreshHapticFileList(); g_current_selected_haptic_file_index = -1; g_haptic_file_load_error[0] = '\0'; g_audio_file_load_error[0] = '\0';}
      ImGui::SameLine(); ImGui::Text("%zu files found in '%s'", g_available_haptic_files.size(), g_haptic_files_directory.c_str());

      const char* combo_preview_value = (g_current_selected_haptic_file_index >= 0 && g_current_selected_haptic_file_index < g_available_haptic_files.size())
                                      ? g_available_haptic_files[g_current_selected_haptic_file_index].c_str() : "Select a haptic file...";
      if (ImGui::BeginCombo("Haptic File", combo_preview_value, ImGuiComboFlags_HeightLargest)) {
          for (int n = 0; n < g_available_haptic_files.size(); n++) {
              const bool is_selected = (g_current_selected_haptic_file_index == n);
              if (ImGui::Selectable(g_available_haptic_files[n].c_str(), is_selected)) {
                  g_current_selected_haptic_file_index = n; g_haptic_file_load_error[0] = '\0'; g_audio_file_load_error[0] = '\0';
              }
              if (is_selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
      }

      bool can_play = (g_current_selected_haptic_file_index != -1 && !g_playback_active);
      if (!can_play) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f); ImGui::BeginDisabled(); }
      if (ImGui::Button("Play")) {
          if (g_current_selected_haptic_file_index != -1) { 
                bool haptics_struct_loaded_successfully = LoadHapticEvents(g_available_haptic_files[g_current_selected_haptic_file_index]);
                bool audio_loaded_successfully = false;

                if (haptics_struct_loaded_successfully) { 
                    audio_loaded_successfully = LoadAndPrepareAudio(g_available_haptic_files[g_current_selected_haptic_file_index]);
                }

                if (haptics_struct_loaded_successfully && (!g_scheduled_events.empty() || audio_loaded_successfully)) {
                    g_playback_active = true;
                    g_playback_start_time_global = g_timeSinceStart;
                    g_next_event_index = 0;
                    g_currently_playing_file = g_available_haptic_files[g_current_selected_haptic_file_index];
                    ImGui::DebugLog("Playback started for: %s\n", g_currently_playing_file.c_str());
                    if (haptics_struct_loaded_successfully) g_haptic_file_load_error[0] = '\0';
                    if (audio_loaded_successfully) g_audio_file_load_error[0] = '\0';

                    if (g_is_current_song_sound_initialized && audio_loaded_successfully) { 
                        ma_sound_seek_to_pcm_frame(&g_current_song_sound, 0); // Ensure starts from beginning
                        ma_sound_start(&g_current_song_sound);
                    }
                } else {
                    if (!haptics_struct_loaded_successfully) {
                        ImGui::DebugLog("Failed to load haptic file structure: %s\n", g_available_haptic_files[g_current_selected_haptic_file_index].c_str());
                    } else if (g_scheduled_events.empty() && !audio_loaded_successfully) {
                        ImGui::DebugLog("Haptic file for %s is empty AND audio failed to load. Nothing to play.\n", g_available_haptic_files[g_current_selected_haptic_file_index].c_str());
                    }
                }
          }
      }
      if (!can_play) { ImGui::EndDisabled(); ImGui::PopStyleVar(); }
      
      ImGui::SameLine();

      bool can_stop = g_playback_active; 
      if (!can_stop) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f); ImGui::BeginDisabled(); }
      if (ImGui::Button("Stop")) { 
          if (g_playback_active) { 
              g_playback_active = false;
              ImGui::DebugLog("Playback stopped for: %s\n", g_currently_playing_file.c_str());
              StopAndUnloadAudio(); 
              g_currently_playing_file = "";
          }
      }
      if (!can_stop) { ImGui::EndDisabled(); ImGui::PopStyleVar(); }

      ImGui::Text("Status: %s", g_playback_active ? ("Playing: " + g_currently_playing_file).c_str() : "Stopped");
      if (g_playback_active) {
          ImGui::SameLine();
          double total_duration = g_scheduled_events.empty() ? 0.0 : g_scheduled_events.back().timestamp;
          if (g_is_current_song_sound_initialized) { 
                float audio_len_sec = 0.0f;
                ma_sound_get_length_in_seconds(&g_current_song_sound, &audio_len_sec);
                if (audio_len_sec > total_duration) total_duration = audio_len_sec;
          }
          if (total_duration < (g_timeSinceStart - g_playback_start_time_global) && total_duration > 0) total_duration = (g_timeSinceStart - g_playback_start_time_global);
          ImGui::Text("Time: %.2f / %.2f s", (g_timeSinceStart - g_playback_start_time_global), total_duration);
          float progress = (total_duration > 0.001) ? (float)((g_timeSinceStart - g_playback_start_time_global) / total_duration) : 0.0f;
          ImGui::ProgressBar(min(1.0f, max(0.0f, progress)), ImVec2(-1.0f, 0.0f));
      }
       if (g_haptic_file_load_error[0] != '\0') { ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f), "Haptic Error: %s", g_haptic_file_load_error); }
       if (g_audio_file_load_error[0] != '\0') { ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f), "Audio Error: %s", g_audio_file_load_error); }
      ImGui::End();
    }

    // Debug Log Window
    {
      ImGui::SetNextWindowPos(ImVec2(0.f, screenSize.y * 0.7f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x, screenSize.y * 0.3f));
      ImGui::ShowDebugLogWindow();
    }

    ImGui::Render();
    const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    HRESULT hr = g_pSwapChain->Present(1, 0); 
    g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

    if (!g_playback_active && immediateMode) {
        double current_time = g_timeSinceStart;
        auto process_hand_manual = [&](serialib& hand_serial, std::vector<FingerConfig>& fingers_vec, const char* hand_tag) {
            if (hand_serial.isDeviceOpen()) {
                for (FingerConfig &finger : fingers_vec) { 
                    if (finger.Strength > 0 && (finger.LastWriteTime + g_immediateModeDuration < current_time || finger.LastWriteTime == 0.0)) {
                        uint8_t writeBuffer[8]; writeBuffer[0] = static_cast<uint8_t>(finger.Location); writeBuffer[1] = static_cast<uint8_t>(finger.Strength);
                        float duration_to_send = g_immediateModeDuration; std::memcpy(&writeBuffer[2], &duration_to_send, sizeof(float)); std::memcpy(&writeBuffer[6], paddingBuffer, 2);
                        if (hand_serial.writeBytes(writeBuffer, 8) > 0) finger.LastWriteTime = current_time;
                        else ImGui::DebugLog("%s: Write Fail %s\n", hand_tag, GetFingerText(finger.Location).c_str());
                    }
                }
            }
        };
        process_hand_manual(leftHand, g_leftHandFingers, "L");
        process_hand_manual(rightHand, g_rightHandFingers, "R");
    }

    if (g_playback_active) {
        double current_playback_elapsed_time = g_timeSinceStart - g_playback_start_time_global;
        bool all_haptics_done = true; 

        if (g_next_event_index < g_scheduled_events.size()) {
            all_haptics_done = false; 
            while (g_next_event_index < g_scheduled_events.size() && current_playback_elapsed_time >= g_scheduled_events[g_next_event_index].timestamp) {
                const HapticEvent& event = g_scheduled_events[g_next_event_index];
                serialib* target_hand_serial = nullptr; const char* hand_name_debug = "";
                if (event.hand_id == 0 && leftHand.isDeviceOpen()) { target_hand_serial = &leftHand; hand_name_debug = "Left"; }
                else if (event.hand_id == 1 && rightHand.isDeviceOpen()) { target_hand_serial = &rightHand; hand_name_debug = "Right"; }

                if (target_hand_serial) {
                    uint8_t writeBuffer[8]; writeBuffer[0] = event.finger_id; writeBuffer[1] = event.strength;
                    float duration_to_send = event.duration; std::memcpy(&writeBuffer[2], &duration_to_send, sizeof(float)); std::memcpy(&writeBuffer[6], paddingBuffer, 2);
                    if (target_hand_serial->writeBytes(writeBuffer, 8) == 0) {
                        ImGui::DebugLog("Playback: Failed to write to %s Hand for event at %.3fs (F:%d,S:%d,D:%.3f)\n", hand_name_debug, event.timestamp, event.finger_id, event.strength, event.duration);
                    }
                }
                g_next_event_index++;
            }
            if(g_next_event_index >= g_scheduled_events.size()){
                all_haptics_done = true; 
            }
        }


        bool audio_still_playing = false;
        if (g_is_current_song_sound_initialized) {
            audio_still_playing = ma_sound_is_playing(&g_current_song_sound);
        }

        if (all_haptics_done && (!g_is_current_song_sound_initialized || !audio_still_playing)) {
            if (!g_scheduled_events.empty() || g_is_current_song_sound_initialized) { 
                 ImGui::DebugLog("Playback automatically finished for %s.\n", g_currently_playing_file.c_str());
            }
            g_playback_active = false; 
            StopAndUnloadAudio();
            g_currently_playing_file = "";
        }
    }
  } 

  ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
  
  StopAndUnloadAudio(); 
  ma_engine_uninit(&g_audio_engine); 

  if(leftHand.isDeviceOpen()) leftHand.closeDevice(); if(rightHand.isDeviceOpen()) rightHand.closeDevice();
  CleanupDeviceD3D(); ::DestroyWindow(hwnd); ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}

void RefreshHapticFileList() {
    g_available_haptic_files.clear(); g_haptic_file_load_error[0] = '\0'; g_audio_file_load_error[0] = '\0';
    try {
        fs::path current_path = fs::current_path(); fs::path haptic_dir_path = current_path / g_haptic_files_directory;
        if (!fs::exists(haptic_dir_path) || !fs::is_directory(haptic_dir_path)) {
            std::string err_msg = "Haptic output directory '" + g_haptic_files_directory + "' not found.";
            strncpy_s(g_haptic_file_load_error, err_msg.c_str(), sizeof(g_haptic_file_load_error) - 1);
            ImGui::DebugLog("%s\n", err_msg.c_str()); return;
        }
        for (const auto& entry : fs::directory_iterator(haptic_dir_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                g_available_haptic_files.push_back(entry.path().filename().string());
            }
        }
        std::sort(g_available_haptic_files.begin(), g_available_haptic_files.end());
        ImGui::DebugLog("Refreshed haptic file list. Found %zu files.\n", g_available_haptic_files.size());
    } catch (const fs::filesystem_error& e) {
        std::string err_msg = "Filesystem error: " + std::string(e.what());
        strncpy_s(g_haptic_file_load_error, err_msg.c_str(), sizeof(g_haptic_file_load_error) - 1);
        ImGui::DebugLog("%s\n", err_msg.c_str());
    } catch (const std::exception& e) {
        std::string err_msg = "Exception: " + std::string(e.what());
        strncpy_s(g_haptic_file_load_error, err_msg.c_str(), sizeof(g_haptic_file_load_error) - 1);
        ImGui::DebugLog("%s\n", err_msg.c_str());
    }
}

bool LoadHapticEvents(const std::string& haptic_filename_without_path) {
    g_scheduled_events.clear(); g_haptic_file_load_error[0] = '\0';
    fs::path file_path = fs::current_path() / g_haptic_files_directory / haptic_filename_without_path;
    std::ifstream file_stream(file_path);
    if (!file_stream.is_open()) {
        std::string err_msg = "Failed to open haptic file: " + file_path.string();
        strncpy_s(g_haptic_file_load_error, err_msg.c_str(), sizeof(g_haptic_file_load_error) -1); return false;
    }
    try {
        json j; file_stream >> j;
        if (!j.is_array()) { strncpy_s(g_haptic_file_load_error, "Haptic file is not a JSON array.", sizeof(g_haptic_file_load_error) -1); return false; }
        for (const auto& item : j) {
            HapticEvent event;
            if (!item.contains("timestamp") || !item["timestamp"].is_number()) { ImGui::DebugLog("Skipping event: missing or invalid timestamp.\n"); continue; }
            event.timestamp = item["timestamp"].get<double>();
            if (!item.contains("hand_id") || !item["hand_id"].is_number_integer()) { ImGui::DebugLog("Skipping event at %.3f: missing or invalid hand_id.\n", event.timestamp); continue; }
            event.hand_id = item["hand_id"].get<int>();
            if (!item.contains("finger_id") || !item["finger_id"].is_number_integer()) { ImGui::DebugLog("Skipping event at %.3f: missing or invalid finger_id.\n", event.timestamp); continue; }
            event.finger_id = item["finger_id"].get<uint8_t>();
            if (!item.contains("strength") || !item["strength"].is_number_integer()) { ImGui::DebugLog("Skipping event at %.3f: missing or invalid strength.\n", event.timestamp); continue; }
            event.strength = item["strength"].get<uint8_t>();
            if (!item.contains("duration") || !item["duration"].is_number()) { ImGui::DebugLog("Skipping event at %.3f: missing or invalid duration.\n", event.timestamp); continue; }
            event.duration = item["duration"].get<float>();
            g_scheduled_events.push_back(event);
        }
        std::sort(g_scheduled_events.begin(), g_scheduled_events.end());
        ImGui::DebugLog("Loaded %zu haptic events from %s.\n", g_scheduled_events.size(), haptic_filename_without_path.c_str());
        if(g_scheduled_events.empty() && j.is_array() && !j.empty()){ 
             strncpy_s(g_haptic_file_load_error, "Haptic file parsed but no valid events found (check format).", sizeof(g_haptic_file_load_error) -1);
        }
        return true;
    } catch (const json::parse_error& e) {
        std::string err_msg = "JSON parse error: " + std::string(e.what()) + " at byte " + std::to_string(e.byte);
        strncpy_s(g_haptic_file_load_error, err_msg.c_str(), sizeof(g_haptic_file_load_error) -1); return false;
    } catch (const json::type_error& e) {
        std::string err_msg = "JSON type error: " + std::string(e.what());
        strncpy_s(g_haptic_file_load_error, err_msg.c_str(), sizeof(g_haptic_file_load_error) -1); return false;
    } catch (const std::exception& e) {
        std::string err_msg = "Error loading events: " + std::string(e.what());
        strncpy_s(g_haptic_file_load_error, err_msg.c_str(), sizeof(g_haptic_file_load_error) -1); return false;
    }
}

bool LoadAndPrepareAudio(const std::string& haptic_filename_without_path) {
    StopAndUnloadAudio(); 
    g_audio_file_load_error[0] = '\0'; 

    std::string base_filename = haptic_filename_without_path;
    size_t pos = base_filename.rfind("_haptics.json");
    if (pos != std::string::npos) {
        base_filename.erase(pos);
    } else { 
        pos = base_filename.rfind(".json");
        if (pos != std::string::npos) {
            base_filename.erase(pos);
        }
    }
    
    fs::path audio_path_wav = fs::current_path() / g_audio_files_directory / (base_filename + ".wav");
    fs::path audio_path_mp3 = fs::current_path() / g_audio_files_directory / (base_filename + ".mp3");
    fs::path audio_path_to_load;

    bool found_audio = false;
    if (fs::exists(audio_path_wav)) {
        audio_path_to_load = audio_path_wav;
        found_audio = true;
    } else if (fs::exists(audio_path_mp3)) {
        audio_path_to_load = audio_path_mp3;
        found_audio = true;
    }

    if (!found_audio) {
        std::string err_msg = "Audio file not found for " + base_filename + " (.wav or .mp3 in '" + g_audio_files_directory + "' folder).";
        strncpy_s(g_audio_file_load_error, err_msg.c_str(), sizeof(g_audio_file_load_error) - 1);
        ImGui::DebugLog("%s\n", err_msg.c_str());
        return false;
    }

    ma_uint32 flags = MA_SOUND_FLAG_DECODE; // Explicitly decode the whole file
    ma_result result = ma_sound_init_from_file(&g_audio_engine, audio_path_to_load.string().c_str(), flags, NULL, NULL, &g_current_song_sound);
    if (result != MA_SUCCESS) {
        std::string err_msg = "Failed to load audio file '" + audio_path_to_load.filename().string() + "': " + ma_result_description(result);
        strncpy_s(g_audio_file_load_error, err_msg.c_str(), sizeof(g_audio_file_load_error) - 1);
        ImGui::DebugLog("%s\n", err_msg.c_str());
        g_is_current_song_sound_initialized = false;
        return false;
    }

    ImGui::DebugLog("Audio file loaded: %s\n", audio_path_to_load.filename().string().c_str());
    g_is_current_song_sound_initialized = true;
    return true;
}

void StopAndUnloadAudio() {
    if (g_is_current_song_sound_initialized) {
        if (ma_sound_is_playing(&g_current_song_sound)) {
            ma_sound_stop(&g_current_song_sound);
        }
        ma_sound_uninit(&g_current_song_sound); 
        g_is_current_song_sound_initialized = false;
        ImGui::DebugLog("Audio unloaded.\n");
    }
}


// --- D3D and Window Helper Functions ---
bool CreateDeviceD3D(HWND hWnd) { 
  DXGI_SWAP_CHAIN_DESC sd; ZeroMemory(&sd, sizeof(sd)); sd.BufferCount = 2; sd.BufferDesc.Width = 0; sd.BufferDesc.Height = 0; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1; sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  UINT createDeviceFlags = 0; D3D_FEATURE_LEVEL featureLevel; const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
  HRESULT res = D3D11CreateDeviceAndSwapChain( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res == DXGI_ERROR_UNSUPPORTED) res = D3D11CreateDeviceAndSwapChain( nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res != S_OK) return false; CreateRenderTarget(); return true;
}
void CleanupDeviceD3D() { CleanupRenderTarget(); if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; } if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; } if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; } }
void CreateRenderTarget() { ID3D11Texture2D* pBackBuffer = nullptr; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)); if(pBackBuffer) { g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView); pBackBuffer->Release(); } }
void CleanupRenderTarget() { if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; } }
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {   if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true; switch (msg) { case WM_SIZE: if (wParam == SIZE_MINIMIZED) return 0; g_ResizeWidth = (UINT)LOWORD(lParam); g_ResizeHeight = (UINT)HIWORD(lParam); return 0; case WM_SYSCOMMAND: if ((wParam & 0xfff0) == SC_KEYMENU) return 0; break; case WM_DESTROY: ::PostQuitMessage(0); return 0; } return ::DefWindowProcW(hWnd, msg, wParam, lParam); }
bool LoadTextureFromMemory(const void *data, size_t data_size, ID3D11ShaderResourceView **out_srv, int *out_width, int *out_height) { 
    int image_width = 0; int image_height = 0; unsigned char *image_data = stbi_load_from_memory((const unsigned char *)data, (int)data_size, &image_width, &image_height, NULL, 4); 
    if (image_data == NULL) { ImGui::DebugLog("STBI failed to load image from memory.\n"); return false; }
    D3D11_TEXTURE2D_DESC desc; ZeroMemory(&desc, sizeof(desc)); desc.Width = image_width; desc.Height = image_height; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.CPUAccessFlags = 0;
    ID3D11Texture2D *pTexture = NULL; D3D11_SUBRESOURCE_DATA subResource; subResource.pSysMem = image_data; subResource.SysMemPitch = desc.Width * 4; subResource.SysMemSlicePitch = 0; 
    HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture); 
    if(FAILED(hr)) { 
        ImGui::DebugLog("D3D11CreateTexture2D failed (0x%08X) for image from memory.\n", hr);
        stbi_image_free(image_data); 
        if(pTexture) pTexture->Release(); 
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc; ZeroMemory(&srvDesc, sizeof(srvDesc)); srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = desc.MipLevels; srvDesc.Texture2D.MostDetailedMip = 0; 
    hr = g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv); 
    if(pTexture) pTexture->Release(); 
    if(FAILED(hr)) { 
        ImGui::DebugLog("D3D11CreateShaderResourceView failed (0x%08X) for image from memory.\n", hr);
        stbi_image_free(image_data); 
        return false;
    }
    *out_width = image_width; *out_height = image_height; stbi_image_free(image_data); return true;
}
bool LoadTextureFromFile(const char *file_name, ID3D11ShaderResourceView **out_srv, int *out_width, int *out_height) {
    FILE *f = fopen(file_name, "rb"); if (f == NULL) { ImGui::DebugLog("Failed to open texture file: %s\n", file_name); return false; } 
    fseek(f, 0, SEEK_END); size_t file_size = (size_t)ftell(f); if (file_size == (size_t)-1 || file_size == 0) {fclose(f); ImGui::DebugLog("Invalid file size for texture: %s\n", file_name); return false;} 
    fseek(f, 0, SEEK_SET); void *file_data = IM_ALLOC(file_size); if (!file_data) { fclose(f); ImGui::DebugLog("Failed to alloc memory for texture: %s\n", file_name); return false; }
    size_t read_size = fread(file_data, 1, file_size, f); fclose(f); if(read_size != file_size) { IM_FREE(file_data); ImGui::DebugLog("Failed to read texture file: %s\n", file_name); return false;}
    bool ret_val = LoadTextureFromMemory(file_data, file_size, out_srv, out_width, out_height); IM_FREE(file_data); return ret_val;
}
std::string GetFingerText(ETargetHandLocation Location) {
  switch (Location) {
  case ETargetHandLocation::Thumb: return "Thumb"; case ETargetHandLocation::Index: return "Index";
  case ETargetHandLocation::Middle: return "Middle"; case ETargetHandLocation::Ring: return "Ring";
  case ETargetHandLocation::Pinky: return "Pinky"; case ETargetHandLocation::None: return "None";
  default: return "Unknown";
  }
}
ImVec4 LerpColorHSV(const ImVec4& srgbColor1, const ImVec4& srgbColor2, float t) {
    ImVec4 result; float r1 = srgbColor1.x, g1 = srgbColor1.y, b1 = srgbColor1.z, a1 = srgbColor1.w;
    float r2 = srgbColor2.x, g2 = srgbColor2.y, b2 = srgbColor2.z, a2 = srgbColor2.w;
    float h1, s1, v1; ImGui::ColorConvertRGBtoHSV(r1, g1, b1, h1, s1, v1);
    float h2, s2, v2; ImGui::ColorConvertRGBtoHSV(r2, g2, b2, h2, s2, v2);
    float s_interp = s1 + (s2 - s1) * t; float v_interp = v1 + (v2 - v1) * t;
    float h_interp; float diff_h = h2 - h1;
    if (s1 == 0.0f) { h_interp = h2; if (s2 == 0.0f) h_interp = 0.0f; } 
    else if (s2 == 0.0f) { h_interp = h1; } 
    else {
        if (diff_h > 0.5f) { h_interp = h1 + (diff_h - 1.0f) * t; }
        else if (diff_h < -0.5f) { h_interp = h1 + (diff_h + 1.0f) * t; }
        else { h_interp = h1 + diff_h * t; }
    }
    h_interp = fmodf(h_interp, 1.0f); if (h_interp < 0.0f) { h_interp += 1.0f; }
    if (s_interp < 0.00001f) { h_interp = 0.0f; } 
    float a_interp = a1 + (a2 - a1) * t;
    float r_interp, g_interp, b_interp;
    ImGui::ColorConvertHSVtoRGB(h_interp, s_interp, v_interp, r_interp, g_interp, b_interp); // Corrected typo here
    return ImVec4(r_interp, g_interp, b_interp, a_interp);
}

