#include <windows.h>
#include <chrono>
#include <thread>
#include "Render/DirectXMain.h"

constexpr UINT WIDTH = 1280;
constexpr UINT HEIGHT = 720;
constexpr wchar_t WINDOW_CLASS_NAMO[] = L"DX12GameWindowClass";
constexpr wchar_t WINDOW_TITLE[] = L"DirectX12 Game Loop";

// 60 FPS target
constexpr auto TARGET_FRAME_TIME = std::chrono::milliseconds(16);

LRESULT CALLBACK WndProc([[maybe_unused]] HWND hwnd, UINT msg, WPARAM wParam, [[maybe_unused]] LPARAM lParam);

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    [[maybe_unused]] _In_opt_ HINSTANCE hPrevInstance,
    [[maybe_unused]] _In_ PWSTR pCmdLine,
    _In_ int nCmdShow)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!wc.hCursor) {
        MessageBoxW(nullptr, L"Failed to load cursor", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS_NAMO;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Window class registration failed", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    RECT windowRect = { 0, 0, static_cast<LONG>(WIDTH), static_cast<LONG>(HEIGHT) };
    if (!AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE)) {
        MessageBoxW(nullptr, L"Window rect adjustment failed", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAMO,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hwnd) {
        MessageBoxW(nullptr, L"Window creation failed", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    if (!InitD3D12(hwnd, WIDTH, HEIGHT)) {
        MessageBoxW(nullptr, L"DirectX12 initialization failed", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    bool isRunning = true;
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (isRunning) {
        // Process all pending messages
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                isRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (isRunning) {
            auto currentTime = std::chrono::steady_clock::now();
            auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                currentTime - lastFrameTime);

            // Frame rate limiting for 60 FPS
            if (deltaTime >= TARGET_FRAME_TIME) {
                Update();
                Render();
                lastFrameTime = currentTime;
            }
            else {
                // Yield CPU time to avoid busy-waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    WaitForPreviousFrame();
    CleanupD3D12();

    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc([[maybe_unused]] HWND hwnd, UINT msg, WPARAM wParam, [[maybe_unused]] LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
        }
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int main()
{
    return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWDEFAULT);
}
