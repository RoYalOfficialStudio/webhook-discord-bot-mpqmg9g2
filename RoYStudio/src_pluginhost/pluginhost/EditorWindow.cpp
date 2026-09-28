#include "pluginhost/EditorWindow.h"

#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <poll.h>
#endif

namespace roy::pluginhost {

namespace {
double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

// ------------------------------------------------------------------ RunLoop
int RunLoop::addTimer(int periodMs, std::function<void()> fn) {
    const int id = nextId_++;
    timers_.push_back({id, periodMs < 1 ? 1 : periodMs, nowMs() + periodMs, std::move(fn)});
    return id;
}
void RunLoop::removeTimer(int id) {
    for (size_t i = 0; i < timers_.size(); ++i)
        if (timers_[i].id == id) {
            timers_.erase(timers_.begin() + static_cast<long>(i));
            return;
        }
}
int RunLoop::addFd(int fd, std::function<void()> fn) {
    const int id = nextId_++;
    fds_.push_back({id, fd, std::move(fn)});
    return id;
}
void RunLoop::removeFd(int id) {
    for (size_t i = 0; i < fds_.size(); ++i)
        if (fds_[i].id == id) {
            fds_.erase(fds_.begin() + static_cast<long>(i));
            return;
        }
}
void RunLoop::run() {
    const double now = nowMs();
    // copy ids first: callbacks may add/remove timers
    std::vector<int> due;
    for (auto& t : timers_)
        if (t.next <= now) due.push_back(t.id);
    for (int id : due)
        for (auto& t : timers_)
            if (t.id == id) {
                t.next = now + t.periodMs;
                auto fn = t.fn;
                fn();
                break;
            }
#ifndef _WIN32
    std::vector<std::pair<int, int>> ready;
    for (auto& f : fds_) {
        pollfd p{f.fd, POLLIN, 0};
        if (::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) ready.push_back({f.id, f.fd});
    }
    for (auto& [id, fd] : ready)
        for (auto& f : fds_)
            if (f.id == id) {
                auto fn = f.fn;
                fn();
                break;
            }
#endif
}

// ------------------------------------------------------------------ EditorWindow
EditorWindow::EditorWindow() = default;
EditorWindow::~EditorWindow() { destroy(); }

#ifdef _WIN32
namespace {
const wchar_t* kClass = L"RoYPluginEditor";
LRESULT CALLBACK editorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_CLOSE:
        if (self && self->onCloseRequested) self->onCloseRequested();
        return 0; // never destroy the plugin from here: the host decides
    case WM_SIZE:
        if (self && self->onUserResized && wp != SIZE_MINIMIZED) self->onUserResized(LOWORD(lp), HIWORD(lp));
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace

bool EditorWindow::create(const std::string& title, int width, int height, bool resizable, bool alwaysOnTop, std::string* error) {
    destroy();
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = editorProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&wc);
        registered = true;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | (resizable ? (WS_THICKFRAME | WS_MAXIMIZEBOX) : 0);
    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    const std::wstring wt(title.begin(), title.end());
    HWND h = CreateWindowExW(alwaysOnTop ? WS_EX_TOPMOST : 0, kClass, wt.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) {
        if (error) *error = "CreateWindowEx failed";
        return false;
    }
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(h, SW_SHOWNORMAL);
    handle_ = h;
    w_ = width;
    h_ = height;
    return true;
}

void EditorWindow::destroy() {
    if (handle_) DestroyWindow(static_cast<HWND>(handle_));
    handle_ = nullptr;
}
const char* EditorWindow::platformType() const { return "HWND"; }
void EditorWindow::resize(int width, int height) {
    if (!handle_) return;
    HWND h = static_cast<HWND>(handle_);
    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(h, GWL_EXSTYLE)));
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    w_ = width;
    h_ = height;
}
double EditorWindow::dpiScale() const {
    if (!handle_) return 1.0;
    using Fn = UINT(WINAPI*)(HWND);
    static Fn getDpi = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return getDpi ? getDpi(static_cast<HWND>(handle_)) / 96.0 : 1.0;
}
void EditorWindow::setAlwaysOnTop(bool on) {
    if (handle_) SetWindowPos(static_cast<HWND>(handle_), on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}
void EditorWindow::focus() {
    if (handle_) SetForegroundWindow(static_cast<HWND>(handle_));
}
void EditorWindow::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
#else
// ------------------------------------------------------------------ X11
bool EditorWindow::create(const std::string& title, int width, int height, bool resizable, bool alwaysOnTop, std::string* error) {
    destroy();
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        if (error) *error = "no X11 display (DISPLAY not set)";
        return false;
    }
    const int screen = DefaultScreen(d);
    ::Window w = XCreateSimpleWindow(d, RootWindow(d, screen), 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height), 0,
                                     BlackPixel(d, screen), BlackPixel(d, screen));
    XStoreName(d, w, title.c_str());
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, w, &del, 1);
    XSelectInput(d, w, StructureNotifyMask);
    if (!resizable) {
        XSizeHints* hints = XAllocSizeHints();
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = width;
        hints->min_height = hints->max_height = height;
        XSetWMNormalHints(d, w, hints);
        XFree(hints);
    }
    XMapWindow(d, w);
    XFlush(d);
    display_ = d;
    wmDelete_ = del;
    handle_ = reinterpret_cast<void*>(static_cast<uintptr_t>(w));
    w_ = width;
    h_ = height;
    if (alwaysOnTop) setAlwaysOnTop(true);
    return true;
}

void EditorWindow::destroy() {
    if (display_) {
        if (handle_) XDestroyWindow(static_cast<Display*>(display_), static_cast<::Window>(reinterpret_cast<uintptr_t>(handle_)));
        XCloseDisplay(static_cast<Display*>(display_));
    }
    display_ = nullptr;
    handle_ = nullptr;
}
const char* EditorWindow::platformType() const { return "X11EmbedWindowID"; }
void EditorWindow::resize(int width, int height) {
    if (!handle_) return;
    XResizeWindow(static_cast<Display*>(display_), static_cast<::Window>(reinterpret_cast<uintptr_t>(handle_)), static_cast<unsigned>(width),
                  static_cast<unsigned>(height));
    XFlush(static_cast<Display*>(display_));
    w_ = width;
    h_ = height;
}
double EditorWindow::dpiScale() const { return 1.0; }
void EditorWindow::setAlwaysOnTop(bool on) {
    if (!handle_) return;
    Display* d = static_cast<Display*>(display_);
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.window = static_cast<::Window>(reinterpret_cast<uintptr_t>(handle_));
    e.xclient.message_type = XInternAtom(d, "_NET_WM_STATE", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = on ? 1 : 0;
    e.xclient.data.l[1] = static_cast<long>(XInternAtom(d, "_NET_WM_STATE_ABOVE", False));
    XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(d);
}
void EditorWindow::focus() {
    if (handle_) XRaiseWindow(static_cast<Display*>(display_), static_cast<::Window>(reinterpret_cast<uintptr_t>(handle_)));
}
void EditorWindow::pump() {
    if (!display_) return;
    Display* d = static_cast<Display*>(display_);
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == ClientMessage && static_cast<unsigned long>(e.xclient.data.l[0]) == wmDelete_) {
            if (onCloseRequested) onCloseRequested();
        } else if (e.type == ConfigureNotify && (e.xconfigure.width != w_ || e.xconfigure.height != h_)) {
            w_ = e.xconfigure.width;
            h_ = e.xconfigure.height;
            if (onUserResized) onUserResized(w_, h_);
        }
    }
}
#endif

} // namespace roy::pluginhost
