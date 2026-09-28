#pragma once
// Native top-level window that hosts a plugin editor inside RoYPluginHost.
// Windows: Win32 window (HWND). Linux: X11 window (Window id, via Xlib).
// The window lives in the sandbox process: a crashing plugin GUI takes down only
// the host process, never RoY Studio.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace roy::pluginhost {

class EditorWindow {
public:
    EditorWindow();
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    bool create(const std::string& title, int width, int height, bool resizable, bool alwaysOnTop, std::string* error);
    void destroy();
    bool isOpen() const { return handle_ != nullptr; }
    // Native handle for the plugin: HWND on Windows, X11 Window id (as pointer-sized int) on Linux.
    void* nativeHandle() const { return handle_; }
    const char* platformType() const; // "HWND" / "X11EmbedWindowID"
    void resize(int width, int height);
    int width() const { return w_; }
    int height() const { return h_; }
    double dpiScale() const;
    void setAlwaysOnTop(bool on);
    void focus();
    // Processes pending window-system events. Main thread.
    void pump();

    std::function<void()> onCloseRequested;       // user pressed the close button
    std::function<void(int, int)> onUserResized;   // user resized the frame

private:
    void* handle_ = nullptr;
    int w_ = 0, h_ = 0;
#ifndef _WIN32
    void* display_ = nullptr;
    unsigned long wmDelete_ = 0;
#endif
};

// Timers / fd handlers requested by plugin GUIs (VST3 Linux::IRunLoop, CLAP timer + posix-fd).
class RunLoop {
public:
    int addTimer(int periodMs, std::function<void()> fn); // returns id
    void removeTimer(int id);
    int addFd(int fd, std::function<void()> fn);
    void removeFd(int id);
    void run(); // fire due timers, dispatch readable fds (non-blocking). Main thread.
    size_t timerCount() const { return timers_.size(); }

private:
    struct Timer { int id; int periodMs; double next; std::function<void()> fn; };
    struct Fd { int id; int fd; std::function<void()> fn; };
    std::vector<Timer> timers_;
    std::vector<Fd> fds_;
    int nextId_ = 1;
};

} // namespace roy::pluginhost
