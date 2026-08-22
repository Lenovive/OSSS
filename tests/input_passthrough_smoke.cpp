#include "renderer.h"

#include <windows.h>
#include <dwmapi.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr wchar_t kTargetClassName[] = L"OSSS.InputPassthroughTarget";
constexpr COLORREF kTargetColor = RGB(36, 210, 96);

struct TargetState {
    std::atomic<HWND> window = nullptr;
    std::atomic<int> clicks = 0;
    std::atomic<bool> failed = false;
};

LRESULT CALLBACK TargetWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    auto* state = reinterpret_cast<TargetState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<TargetState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const HBRUSH brush = CreateSolidBrush(kTargetColor);
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONUP:
        if (state) {
            ++state->clicks;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void RunTargetThread(TargetState& state) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = TargetWindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kTargetClassName;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state.failed = true;
        return;
    }

    const HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kTargetClassName,
        L"OSSS Input Passthrough Target",
        WS_POPUP,
        120,
        120,
        360,
        220,
        nullptr,
        nullptr,
        instance,
        &state);
    if (!window) {
        state.failed = true;
        UnregisterClassW(kTargetClassName, instance);
        return;
    }

    state.window = window;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    state.window = nullptr;
    UnregisterClassW(kTargetClassName, instance);
}

HWND WaitForTarget(TargetState& state) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (state.failed) {
            throw std::runtime_error("The input smoke target could not be created.");
        }
        if (const HWND target = state.window.load()) {
            return target;
        }
        Sleep(10);
    }
    throw std::runtime_error("Timed out while creating the input smoke target.");
}

void SendLeftClick(const POINT point) {
    if (!SetCursorPos(point.x, point.y)) {
        throw std::runtime_error("Could not position the pointer for the input smoke test.");
    }

    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(2, input, sizeof(INPUT)) != 2) {
        throw std::runtime_error("Could not inject the input smoke-test click.");
    }
}

} // namespace

int main() {
    POINT original_cursor{};
    GetCursorPos(&original_cursor);

    TargetState state;
    std::thread target_thread(RunTargetThread, std::ref(state));
    try {
        const HWND target = WaitForTarget(state);
        RECT bounds{};
        if (!GetWindowRect(target, &bounds)) {
            throw std::runtime_error("Could not read the input smoke target bounds.");
        }

        {
            osss::Renderer renderer;
            renderer.InitializeDevice();
            renderer.CreateOutputWindow(bounds, 2, 240.0);
            renderer.Show();
            renderer.Render(1.0F);
            renderer.Present();
            DwmFlush();

            const POINT click_point{
                bounds.left + (bounds.right - bounds.left) / 2,
                bounds.top + (bounds.bottom - bounds.top) / 2,
            };
            const HDC screen = GetDC(nullptr);
            if (!screen) {
                throw std::runtime_error("Could not inspect the generated-frame surface.");
            }
            const COLORREF presented_color = GetPixel(screen, click_point.x, click_point.y);
            ReleaseDC(nullptr, screen);
            if (presented_color == CLR_INVALID ||
                GetRValue(presented_color) > 24 ||
                GetGValue(presented_color) > 24 ||
                GetBValue(presented_color) > 24) {
                throw std::runtime_error(
                    "The generated-frame surface was not visible above the target (sampled RGB " +
                    std::to_string(GetRValue(presented_color)) + "," +
                    std::to_string(GetGValue(presented_color)) + "," +
                    std::to_string(GetBValue(presented_color)) + ").");
            }
            SendLeftClick(click_point);

            const auto click_deadline = std::chrono::steady_clock::now() + 1s;
            while (state.clicks.load() == 0 && std::chrono::steady_clock::now() < click_deadline) {
                (void)renderer.PumpMessages();
                Sleep(10);
            }
        }

        PostMessageW(target, WM_CLOSE, 0, 0);
        target_thread.join();
        SetCursorPos(original_cursor.x, original_cursor.y);

        if (state.clicks.load() != 1) {
            std::cerr << "FAILED: The generated-frame surface intercepted the target click.\n";
            return EXIT_FAILURE;
        }

        std::cout << "OSSS generated-frame input passthrough smoke test passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        if (const HWND target = state.window.load()) {
            PostMessageW(target, WM_CLOSE, 0, 0);
        }
        if (target_thread.joinable()) {
            target_thread.join();
        }
        SetCursorPos(original_cursor.x, original_cursor.y);
        std::cerr << "FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
