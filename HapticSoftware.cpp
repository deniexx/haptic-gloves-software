#include <string>
#define STB_IMAGE_IMPLEMENTATION
#define _CRT_SECURE_NO_WARNINGS
#include "vendor/imgui/backends/imgui_impl_dx11.h"
#include "vendor/imgui/backends/imgui_impl_win32.h"
#include "vendor/imgui/imgui.h"
#include "vendor/seriallib/serialib.h"
#include "vendor/stb_image.h"
#include <cstdint>
#include <d3d11.h>
#include <tchar.h>

// @TODO: (Denis) Add a changing color depending on which finger is playing the feedback
// @TODO: (Denis) Add sample plays for the gloves, like jingle bells

enum class ETargetHandLocation : uint8_t 
{
  Thumb = 0,
  Index = 1,
  Middle = 2,
  Ring = 3,
  Pinky = 4,
  None = 5
};

struct FingerConfig 
{
  ETargetHandLocation Location = ETargetHandLocation::None;
  int Strength = 0;
  float Duration = 0.f;

  double LastWriteTime = 0.0;
};

static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;
static double g_timeSinceStart = 0.0;

static std::string StrengthTitle = "Strength";
static std::string DurationTitle = "Duration";
static bool immediateMode = true;
constexpr float g_immediateModeDuration = 0.2f;
static uint8_t paddingBuffer[2] = {0, 0};

constexpr float dragWidgetWidth = 200.f;
constexpr float imagePadding = 25.f;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool LoadTextureFromMemory(const void *data, size_t data_size,
                           ID3D11ShaderResourceView **out_srv, int *out_width,
                           int *out_height);
bool LoadTextureFromFile(const char *file_name,
                         ID3D11ShaderResourceView **out_srv, int *out_width,
                         int *out_height);
std::string GetFingerText(ETargetHandLocation Location);

ImVec4 LerpColorSimple(const ImVec4& srgbColor1, const ImVec4& srgbColor2, float t);

// Main code
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) 
{
  WNDCLASSEXW wc = {sizeof(wc),
                    CS_CLASSDC,
                    WndProc,
                    0L,
                    0L,
                    GetModuleHandle(nullptr),
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    L"Haptic Software",
                    nullptr};
 
  ::RegisterClassExW(&wc);
  
  HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Haptic Software",
                              WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr,
                              nullptr, wc.hInstance, nullptr);

  if (!CreateDeviceD3D(hwnd)) 
  {
    CleanupDeviceD3D();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  ::ShowWindow(hwnd, SW_SHOWDEFAULT);
  ::UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls

  ImGui::StyleColorsDark();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  ImFont* titleFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 24);
  ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 20);
  ImFont* smallFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 16);
  io.Fonts->Build();
  io.FontDefault = smallFont;
  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
  ImVec4 initialColor = ImVec4(0.2f, 0.4f, 0.92f, 1.f);
  ImVec4 targetColor = ImVec4(1.f, 0.f, 0.f, 1.f);
  ImVec4 transparent = ImVec4(0.f, 0.f, 0.f, 0.f);

  serialib leftHand;
  serialib rightHand;

  if (leftHand.openDevice("\\\\.\\COM6", 9600) != 1) {
    // error here?
  }

  if (rightHand.openDevice("\\\\.\\COM11", 9600) != 1) {
    // error here?
  }

  float imageDownScale = 3.75f;
  int imageWidth = 0;
  int imageHeight = 0;

  ID3D11ShaderResourceView* handTexture = nullptr;
  bool ret = LoadTextureFromFile(".\\Assets\\hand.png", &handTexture, &imageWidth, &imageHeight);
  IM_ASSERT(ret);

  ID3D11ShaderResourceView* indexTexture = nullptr;
  ret = LoadTextureFromFile(".\\Assets\\index.png", &indexTexture, &imageWidth, &imageHeight);
  IM_ASSERT(ret);

  ID3D11ShaderResourceView* middleTexture = nullptr;
  ret = LoadTextureFromFile(".\\Assets\\middle.png", &middleTexture, &imageWidth, &imageHeight);
  IM_ASSERT(ret);

  ID3D11ShaderResourceView* ringTexture = nullptr;
  ret = LoadTextureFromFile(".\\Assets\\ring.png", &ringTexture, &imageWidth, &imageHeight);
  IM_ASSERT(ret);

  ID3D11ShaderResourceView* pinkyTexture = nullptr;
  ret = LoadTextureFromFile(".\\Assets\\pinky.png", &pinkyTexture, &imageWidth, &imageHeight);
  IM_ASSERT(ret);

  ID3D11ShaderResourceView* thumbTexture = nullptr;
  ret = LoadTextureFromFile(".\\Assets\\thumb.png", &thumbTexture, &imageWidth, &imageHeight);
  IM_ASSERT(ret);

  ImVec2 normalUv0(0.f, 0.f);
  ImVec2 normalUv1(1.f, 1.f);
  ImVec2 flipUv0(1.f, 0.f);
  ImVec2 flipUv1(0.f, 1.f);
  float drawImageWidth = imageWidth / imageDownScale;
  float drawImageHeight = imageHeight / imageDownScale;

  FingerConfig leftHandFingers[5] = 
  {
    {ETargetHandLocation::Thumb, 0, 0.f},
    {ETargetHandLocation::Index, 0, 0.f},
    {ETargetHandLocation::Middle, 0, 0.f},
    {ETargetHandLocation::Ring, 0, 0.f},
    {ETargetHandLocation::Pinky, 0, 0.f}
  };

  FingerConfig rightHandFingers[5] = 
  {
      {ETargetHandLocation::Thumb, 0, 0.f},
      {ETargetHandLocation::Index, 0, 0.f},
      {ETargetHandLocation::Middle, 0, 0.f},
      {ETargetHandLocation::Ring, 0, 0.f},
      {ETargetHandLocation::Pinky, 0, 0.f}
  };

  ImGuiStyle * style = &ImGui::GetStyle();
 
  style->WindowPadding = ImVec2(15, 15);
  style->WindowRounding = 5.0f;
  style->FramePadding = ImVec2(5, 5);
  style->FrameRounding = 4.0f;
  style->ItemSpacing = ImVec2(12, 8);
  style->ItemInnerSpacing = ImVec2(8, 6);
  style->IndentSpacing = 25.0f;
  style->ScrollbarSize = 15.0f;
  style->ScrollbarRounding = 9.0f;
  style->GrabMinSize = 5.0f;
  style->GrabRounding = 3.0f;

  style->Colors[ImGuiCol_Text] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
  style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
  style->Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
  style->Colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
  style->Colors[ImGuiCol_Border] = ImVec4(0.80f, 0.80f, 0.83f, 0.88f);
  style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.92f, 0.91f, 0.88f, 0.00f);
  style->Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
  style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
  style->Colors[ImGuiCol_FrameBgActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
  style->Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
  style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(1.00f, 0.98f, 0.95f, 0.75f);
  style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
  style->Colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
  style->Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
  style->Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
  style->Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
  style->Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
  style->Colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
  style->Colors[ImGuiCol_SliderGrab] = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
  style->Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
  style->Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
  style->Colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
  style->Colors[ImGuiCol_ButtonActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
  style->Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
  style->Colors[ImGuiCol_HeaderHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
  style->Colors[ImGuiCol_HeaderActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
  style->Colors[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  style->Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
  style->Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
  style->Colors[ImGuiCol_PlotLines] = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
  style->Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
  style->Colors[ImGuiCol_PlotHistogram] = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
  style->Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
  style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);

  bool done = false;
  while (!done) 
  {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) 
    {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        done = true;
    }
    if (done)
      break;

    // Handle window being minimized or screen locked
    if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) 
    {
      ::Sleep(10);
      continue;
    }
    g_SwapChainOccluded = false;

    // Handle window resize (we don't resize directly in the WM_SIZE handler)
    if (g_ResizeWidth != 0 && g_ResizeHeight != 0) 
    {
      CleanupRenderTarget();
      g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
      g_ResizeWidth = g_ResizeHeight = 0;
      CreateRenderTarget();
    }

    // Start the Dear ImGui frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImVec2 screenSize = io.DisplaySize;

    {
      ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x * 0.2f, screenSize.y * 0.6f));
      ImGui::Begin("Left Hand", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
      if (leftHand.isDeviceOpen() || 1) 
      {
        ImGui::SetCursorPosY(screenSize.y  * 0.05f);
        ImGui::PushFont(titleFont);
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x * 0.5f - ImGui::CalcTextSize("Left Hand").x * 0.4f);
        ImGui::Text("Left Hand");
        ImGui::PopFont();
        ImGui::SetCursorPosY(screenSize.y * 0.125f);
        for (int i = 0; i < 5; ++i) 
        {
          std::string FullTitle = GetFingerText(leftHandFingers[i].Location) + " " + StrengthTitle;
          std::string id = "##lhand" + std::to_string(i);
          float local_x = ImGui::GetContentRegionAvail().x * 0.5f - (ImGui::CalcTextSize(FullTitle.c_str()).x * 0.433f);
          float local_x_widget = ImGui::GetContentRegionAvail().x * 0.5f - (dragWidgetWidth * 0.433f);
          ImGui::SetCursorPosX(local_x);
          ImGui::Text("%s", FullTitle.c_str());
          ImGui::SetCursorPosX(local_x_widget);
          ImGui::PushItemWidth(dragWidgetWidth);
          ImGui::DragInt(id.c_str(), &leftHandFingers[i].Strength, 1.f, 0, 255); 
          ImGui::PopItemWidth();

          if (!immediateMode) 
          {
            ImGui::DragFloat(DurationTitle.c_str(), &leftHandFingers[i].Duration, 0.f, 10.f);
          }
        }
      } 
      else 
      {
        ImGui::PushFont(font);
        static std::string ErrorLeftHandText = "Could not create COM6 port for Left Hand";
        ImVec2 panelSize = ImGui::GetContentRegionAvail();
        ImVec2 size = ImGui::CalcTextSize(ErrorLeftHandText.c_str());
        ImVec2 centerPos = ImVec2(panelSize.x * 0.5f - size.x * 0.5f, panelSize.y * 0.5f - size.y * 0.5f);
        ImGui::SetCursorPos(centerPos);
        ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f), "%s", ErrorLeftHandText.c_str());
        ImGui::PopFont();
      }
      ImGui::End();
    }
    
    {
      // Left Hand
      ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.2f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x * 0.6f, screenSize.y * 0.6f));
      ImGui::Begin("Hand Image", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
      ImVec2 panelSize = ImGui::GetContentRegionAvail();
      ImVec2 imagePos = ImVec2(imagePadding, panelSize.y * 0.5f - drawImageHeight * 0.5f + 25.f);
      ImGui::SetCursorPos(imagePos);
      ImGui::Image((ImTextureID)(intptr_t)handTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, initialColor, transparent);
      ImGui::SetCursorPos(imagePos);
      ImVec4 fingerColor = LerpColorSimple(initialColor, targetColor, leftHandFingers[1].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)indexTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, leftHandFingers[2].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)middleTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, leftHandFingers[3].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)ringTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, leftHandFingers[4].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)pinkyTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, leftHandFingers[0].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)thumbTexture, ImVec2(drawImageWidth, drawImageHeight), normalUv0, normalUv1, fingerColor, transparent);

      // Right Hand
      imagePos = ImVec2(panelSize.x - drawImageWidth * 0.825f - imagePadding, panelSize.y * 0.5f - drawImageHeight * 0.5f + 25.f);
      ImGui::SetCursorPos(imagePos);
      ImGui::Image((ImTextureID)(intptr_t)handTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, initialColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, rightHandFingers[1].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)indexTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, rightHandFingers[2].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)middleTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, rightHandFingers[3].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)ringTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, rightHandFingers[4].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)pinkyTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, fingerColor, transparent);
      ImGui::SetCursorPos(imagePos);
      fingerColor = LerpColorSimple(initialColor, targetColor, rightHandFingers[0].Strength / 255.f);
      ImGui::Image((ImTextureID)(intptr_t)thumbTexture, ImVec2(drawImageWidth, drawImageHeight), flipUv0, flipUv1, fingerColor, transparent);

      // @NOTE: Could add here the playing input

      ImGui::End();
    }

    {
      ImGui::SetNextWindowPos(ImVec2(0.f, screenSize.y * 0.6f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x, screenSize.y * 0.4f));
      ImGui::ShowDebugLogWindow();
    }

    {
      ImGui::SetNextWindowPos(ImVec2(screenSize.x * 0.8f, 0.f));
      ImGui::SetNextWindowSize(ImVec2(screenSize.x * 0.2f, screenSize.y * 0.6f));
      ImGui::Begin("Right Hand", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
      if (rightHand.isDeviceOpen() || 1) 
      {
        ImGui::SetCursorPosY(screenSize.y  * 0.05f);
        ImGui::PushFont(titleFont);
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x * 0.5f - ImGui::CalcTextSize("Right Hand").x * 0.4f);
        ImGui::Text("Right Hand");
        ImGui::PopFont();
        ImGui::SetCursorPosY(screenSize.y * 0.125f);
        for (int i = 0; i < 5; ++i) 
        {
          std::string FullTitle = GetFingerText(rightHandFingers[i].Location) + " " + StrengthTitle;
          std::string id = "##rhand" + std::to_string(i);
          float local_x = ImGui::GetContentRegionAvail().x * 0.5f - (ImGui::CalcTextSize(FullTitle.c_str()).x * 0.433f);
          float local_x_widget = ImGui::GetContentRegionAvail().x * 0.5f - (dragWidgetWidth * 0.433f);
          ImGui::SetCursorPosX(local_x);
          ImGui::Text("%s", FullTitle.c_str());
          ImGui::SetCursorPosX(local_x_widget);
          ImGui::PushItemWidth(dragWidgetWidth);
          ImGui::DragInt(id.c_str(), &rightHandFingers[i].Strength, 1.f, 0, 255); 
          ImGui::PopItemWidth();

          if (!immediateMode) 
          {
            ImGui::DragFloat(DurationTitle.c_str(), &rightHandFingers[i].Duration, 0.f, 10.f);
          }
        }
      } 
      else 
      {
        ImGui::PushFont(font);
        static std::string ErrorRightHandText = "Could not create COM11 port for Right Hand";
        ImVec2 panelSize = ImGui::GetContentRegionAvail();
        ImVec2 size = ImGui::CalcTextSize(ErrorRightHandText.c_str());
        ImVec2 centerPos = ImVec2(panelSize.x * 0.5f - size.x * 0.5f, panelSize.y * 0.5f - size.y * 0.5f);
        ImGui::SetCursorPos(centerPos);
        ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f), "%s", ErrorRightHandText.c_str());
        ImGui::PopFont();
      }
      ImGui::End();
    }

    // Rendering
    ImGui::Render();
    const float clear_color_with_alpha[4] = 
    {
        clear_color.x * clear_color.w, clear_color.y * clear_color.w,
        clear_color.z * clear_color.w, clear_color.w
    };

    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Present
    HRESULT hr = g_pSwapChain->Present(1, 0); // Present with vsync
    // HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
    g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

    g_timeSinceStart += io.DeltaTime;

    if (immediateMode) 
    {
      if (leftHand.isDeviceOpen()) 
      {
        for (FingerConfig &finger : leftHandFingers) 
        {
          if (finger.LastWriteTime + g_immediateModeDuration > g_timeSinceStart || finger.Strength == 0) 
          {
            continue;
          }

          uint8_t writeBuffer[8];
          writeBuffer[0] = static_cast<uint8_t>(finger.Location);
          writeBuffer[1] = static_cast<uint8_t>(finger.Strength);
          std::memcpy(&writeBuffer[2], &g_immediateModeDuration, sizeof(float));
          writeBuffer[6] = paddingBuffer[0];
          writeBuffer[7] = paddingBuffer[1];
          int res = leftHand.writeBytes(writeBuffer, 8);
          if (res == 0)
          {
            ImGui::DebugLog("Failed to write to Left Hand!\n");
          }
          else
          {
            ImGui::DebugLog("Successfully written to Left Hand!\n");
          }
          finger.LastWriteTime = g_timeSinceStart;
        }
      }

      if (rightHand.isDeviceOpen()) 
      {
        for (FingerConfig &finger : rightHandFingers) 
        {
          if (finger.LastWriteTime + g_immediateModeDuration > g_timeSinceStart || finger.Strength == 0) 
          {
            continue;
          }

          uint8_t writeBuffer[8];
          writeBuffer[0] = static_cast<uint8_t>(finger.Location);
          writeBuffer[1] = static_cast<uint8_t>(finger.Strength);
          std::memcpy(&writeBuffer[2], &g_immediateModeDuration, sizeof(float));
          writeBuffer[6] = paddingBuffer[0];
          writeBuffer[7] = paddingBuffer[1];
          int res = rightHand.writeBytes(writeBuffer, 8);
          if (res == 0)
          {
            ImGui::DebugLog("Failed to write to Right Hand!\n");
          }
          else
          {
            ImGui::DebugLog("Successfully written to Right Hand!\n");
          }
          finger.LastWriteTime = g_timeSinceStart;
        }
      }
    }
  }

  // Cleanup
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  leftHand.closeDevice();
  rightHand.closeDevice();

  CleanupDeviceD3D();
  ::DestroyWindow(hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

  return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd) 
{
  // Setup swap chain
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT createDeviceFlags = 0;
  // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray[2] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };
  HRESULT res = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
      featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
      &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software
                                     // driver if hardware is not available.
    res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res != S_OK)
    return false;

  CreateRenderTarget();
  return true;
}

void CleanupDeviceD3D() 
{
  CleanupRenderTarget();
  if (g_pSwapChain) 
  {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) 
  {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) 
  {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}

void CreateRenderTarget() 
{
  ID3D11Texture2D *pBackBuffer;
  g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
  g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
  pBackBuffer->Release();
}

void CleanupRenderTarget() 
{
  if (g_mainRenderTargetView) 
  {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if
// dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your
// main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to
// your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from
// your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) 
{
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) 
  {
  case WM_SIZE:
    if (wParam == SIZE_MINIMIZED)
      return 0;
    g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
    g_ResizeHeight = (UINT)HIWORD(lParam);
    return 0;
  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
      return 0;
    break;
  case WM_DESTROY:
    ::PostQuitMessage(0);
    return 0;
  }
  return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool LoadTextureFromMemory(const void *data, size_t data_size,
                           ID3D11ShaderResourceView **out_srv, int *out_width,
                           int *out_height) 
{
  // Load from disk into a raw RGBA buffer
  int image_width = 0;
  int image_height = 0;
  unsigned char *image_data =
      stbi_load_from_memory((const unsigned char *)data, (int)data_size,
                            &image_width, &image_height, NULL, 4);
  if (image_data == NULL)
    return false;

  // Create texture
  D3D11_TEXTURE2D_DESC desc;
  ZeroMemory(&desc, sizeof(desc));
  desc.Width = image_width;
  desc.Height = image_height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = 0;

  ID3D11Texture2D *pTexture = NULL;
  D3D11_SUBRESOURCE_DATA subResource;
  subResource.pSysMem = image_data;
  subResource.SysMemPitch = desc.Width * 4;
  subResource.SysMemSlicePitch = 0;
  g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);

  // Create texture view
  D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
  ZeroMemory(&srvDesc, sizeof(srvDesc));
  srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = desc.MipLevels;
  srvDesc.Texture2D.MostDetailedMip = 0;
  g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
  pTexture->Release();

  *out_width = image_width;
  *out_height = image_height;
  stbi_image_free(image_data);

  return true;
}

// Open and read a file, then forward to LoadTextureFromMemory()
bool LoadTextureFromFile(const char *file_name,
                         ID3D11ShaderResourceView **out_srv, int *out_width,
                         int *out_height) 
{
  FILE *f = fopen(file_name, "rb");
  if (f == NULL)
    return false;
  fseek(f, 0, SEEK_END);
  size_t file_size = (size_t)ftell(f);
  if (file_size == -1)
    return false;
  fseek(f, 0, SEEK_SET);
  void *file_data = IM_ALLOC(file_size);
  fread(file_data, 1, file_size, f);
  fclose(f);
  bool ret = LoadTextureFromMemory(file_data, file_size, out_srv, out_width,
                                   out_height);
  IM_FREE(file_data);
  return ret;
}

std::string GetFingerText(ETargetHandLocation Location) 
{
  switch (Location) 
  {
  case ETargetHandLocation::Thumb:
    return "Thumb";
  case ETargetHandLocation::Index:
    return "Index";
  case ETargetHandLocation::Middle:
    return "Middle";
  case ETargetHandLocation::Ring:
    return "Ring";
  case ETargetHandLocation::Pinky:
    return "Pinky";
  case ETargetHandLocation::None:
    return "None";
  }

  return "";
}

ImVec4 LerpColorSimple(const ImVec4& srgbColor1, const ImVec4& srgbColor2, float t) 
{
    ImVec4 result;
    float r1 = srgbColor1.x, g1 = srgbColor1.y, b1 = srgbColor1.z, a1 = srgbColor1.w;
    float r2 = srgbColor2.x, g2 = srgbColor2.y, b2 = srgbColor2.z, a2 = srgbColor2.w;

    // Convert sRGB colors to HSV
    float h1, s1, v1;
    ImGui::ColorConvertRGBtoHSV(r1, g1, b1, h1, s1, v1);
    float h2, s2, v2;
    ImGui::ColorConvertRGBtoHSV(r2, g2, b2, h2, s2, v2);

    // Interpolate Saturation and Value linearly
    float s_interp = s1 + (s2 - s1) * t;
    float v_interp = v1 + (v2 - v1) * t;

    // Interpolate Hue (H) with shortest path logic
    // Hue is in the range [0.0, 1.0] from ImGui's conversion
    float h_interp;
    float diff_h = h2 - h1;

    if (s1 == 0.0f) { // Start color is grayscale, its hue is undefined or 0
        h_interp = h2; // Use hue of the target color, or let it interpolate from 0 if h2 is also 0
        if (s2 == 0.0f) h_interp = 0.0f; // Both are grayscale, hue is 0
    } else if (s2 == 0.0f) { // End color is grayscale
        h_interp = h1; // Use hue of the start color
    } else {
        // Both colors have saturation, so interpolate hue
        if (diff_h > 0.5f) {      // e.g., h1=0.1, h2=0.9 -> diff=0.8. Shortest path is -0.2
            h_interp = h1 + (diff_h - 1.0f) * t;
        } else if (diff_h < -0.5f) { // e.g., h1=0.9, h2=0.1 -> diff=-0.8. Shortest path is +0.2
            h_interp = h1 + (diff_h + 1.0f) * t;
        } else {
            h_interp = h1 + diff_h * t;
        }
    }

    // Normalize Hue to [0.0, 1.0) range
    h_interp = fmodf(h_interp, 1.0f);
    if (h_interp < 0.0f) {
        h_interp += 1.0f;
    }
    
    // If interpolated saturation is effectively zero, hue is irrelevant for the final RGB color
    if (s_interp < 0.00001f) {
        h_interp = 0.0f; // Set to a defined value, though it won't affect the color if S=0
    }


    // Interpolate Alpha component linearly
    float a_interp = a1 + (a2 - a1) * t;

    // Convert interpolated HSV back to sRGB
    float r_interp, g_interp, b_interp;
    ImGui::ColorConvertHSVtoRGB(h_interp, s_interp, v_interp, r_interp, g_interp, b_interp);

    return ImVec4(r_interp, g_interp, b_interp, a_interp);
}
