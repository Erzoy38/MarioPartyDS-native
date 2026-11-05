// Phase 0 toolchain check: opens a window with the DS's two screens as empty areas.
// No game code runs here yet, and nothing in this file reads the ROM.
//
// --smoke-test closes the window by itself after the first paint and exits with 0, or 1 if
// the window could not be created. The automated check in native/build.ps1 relies on that.
#include <windows.h>
#include <shellapi.h>

#include <cwchar>

namespace {

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;
constexpr int kScale = 2;
constexpr int kGap = 8;
constexpr UINT_PTR kSmokeTimer = 1;

bool g_smoke_test = false;
bool g_painted = false;

void PaintScreens(HDC dc) {
    HBRUSH screen = CreateSolidBrush(RGB(32, 32, 40));
    const RECT top{0, 0, kScreenWidth * kScale, kScreenHeight * kScale};
    const RECT bottom{0, top.bottom + kGap, top.right, top.bottom + kGap + kScreenHeight * kScale};
    FillRect(dc, &top, screen);
    FillRect(dc, &bottom, screen);
    DeleteObject(screen);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            PaintScreens(dc);
            EndPaint(window, &paint);
            g_painted = true;
            if (g_smoke_test) {
                SetTimer(window, kSmokeTimer, 200, nullptr);
            }
            return 0;
        }
        case WM_TIMER:
            if (wparam == kSmokeTimer) {
                DestroyWindow(window);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool HasArgument(const wchar_t* wanted) {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool found = false;
    for (int i = 1; args != nullptr && i < count; ++i) {
        found = found || std::wcscmp(args[i], wanted) == 0;
    }
    LocalFree(args);
    return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_smoke_test = HasArgument(L"--smoke-test");

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    window_class.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    window_class.lpszClassName = L"MarioPartyDS";
    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    RECT frame{0, 0, kScreenWidth * kScale, kScreenHeight * kScale * 2 + kGap};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&frame, style, FALSE);

    HWND window = CreateWindowExW(0, window_class.lpszClassName,
                                  L"Mario Party DS (native, noch ohne Spiel)", style, CW_USEDEFAULT,
                                  CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
                                  nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }
    ShowWindow(window, g_smoke_test ? SW_SHOWNOACTIVATE : show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return g_smoke_test && !g_painted ? 1 : 0;
}
