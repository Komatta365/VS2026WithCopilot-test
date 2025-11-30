#include <windows.h>
#include <exception>
#include "Render/DirectXMain.h"

constexpr UINT WIDTH = 1280;
constexpr UINT HEIGHT = 720;

bool g_isRunning = true;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    const wchar_t CLASS_NAME[] = L"DX12GameWindowClass";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        return 0;
    }

    RECT windowRect = { 0, 0, static_cast<LONG>(WIDTH), static_cast<LONG>(HEIGHT) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"DirectX12 Game Loop",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        return 0;
    }

    try {
        InitD3D12(hwnd, WIDTH, HEIGHT);
    }
    catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "DirectX12 Initialization Failed", MB_OK | MB_ICONERROR);
        return -1;
    }

    ShowWindow(hwnd, nCmdShow ? nCmdShow : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Game loop with PeekMessage for non-blocking message processing
    MSG msg = {};
    while (g_isRunning) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_isRunning = false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else {
            // Game update and render when no messages to process
            Update();
            Render();
        }
    }

    WaitForPreviousFrame();
    CleanupD3D12();

    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
