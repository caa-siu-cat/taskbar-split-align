#include <windows.h>

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER || message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = TestWindowProc;
    cls.lpszClassName = L"TaskbarSplitAlignmentTest";
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Taskbar alignment test (auto closes)",
                                  WS_OVERLAPPEDWINDOW, 20, 20, 360, 140,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetTimer(window, 1, 4500, nullptr);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
