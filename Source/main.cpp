#include <windows.h>

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Unicode wWinMain: create a simple window and run a message loop
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    const wchar_t CLASS_NAME[] = L"SampleWindowClass";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassExW(&wc))
    {
        return 0;
    }

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Minimal Window",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
    {
        return 0;
    }

    ShowWindow(hwnd, nCmdShow ? nCmdShow : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}

// Minimal window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Parameters:
    //  - hwnd: handle to the window receiving the message
    //  - msg: message identifier (e.g., WM_DESTROY, WM_PAINT)
    //  - wParam/lParam: additional message-specific information
    switch (msg)
    {
    case WM_DESTROY:
        // Sent when a window is being destroyed (after it is removed from the screen).
        // PostQuitMessage posts a WM_QUIT message to the message queue, which causes
        // GetMessage/PeekMessage loops to return false and allows the application to exit.
        PostQuitMessage(0);
        return 0;
    default:
        // For any messages we do not explicitly handle, call DefWindowProc which
        // implements default behavior (keyboard/mouse handling, painting, system commands, etc.).
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Console-friendly entry that forwards to wWinMain for projects set to Console subsystem
int main()
{
    return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWDEFAULT);
}
