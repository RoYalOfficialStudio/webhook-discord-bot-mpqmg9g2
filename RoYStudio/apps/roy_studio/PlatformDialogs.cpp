#include "PlatformDialogs.h"

#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace roy::gui {

namespace {
struct DialogState {
    std::mutex mutex;
    std::atomic<bool> running{false};
    bool ready = false;
    std::string tag;
    std::vector<std::string> paths;
    void* owner = nullptr;
};
DialogState& state() {
    static DialogState* s = new DialogState; // never destroyed: a detached dialog thread may outlive main()
    return *s;
}
} // namespace

void setDialogOwner(void* nativeWindow) { state().owner = nativeWindow; }
bool dialogRunning() { return state().running.load(); }

bool takeDialogResult(std::string& tag, std::vector<std::string>& paths) {
    auto& s = state();
    std::lock_guard l(s.mutex);
    if (!s.ready) return false;
    tag = std::move(s.tag);
    paths = std::move(s.paths);
    s.ready = false;
    return true;
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(w.size() > 0 ? w.size() - 1 : 0);
    return w;
}
std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::string itemPath(IShellItem* item) {
    std::string out;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = narrow(path);
        CoTaskMemFree(path);
    }
    return out;
}

// Runs on the dialog thread (single-threaded apartment).
std::vector<std::string> runDialog(DialogKind kind, const std::string& startFolder, const std::string& title, HWND owner) {
    std::vector<std::string> result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    if (kind == DialogKind::Folder) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    } else {
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
        const COMDLG_FILTERSPEC types[] = {{L"Audio (MP3, WAV, FLAC, OGG, AIFF)", L"*.mp3;*.wav;*.flac;*.ogg;*.aif;*.aiff"},
                                           {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, types);
    }
    const std::wstring t = widen(title);
    dlg->SetTitle(t.c_str());
    IShellItem* start = nullptr;
    if (!startFolder.empty() && SUCCEEDED(SHCreateItemFromParsingName(widen(startFolder).c_str(), nullptr, IID_PPV_ARGS(&start)))) {
        dlg->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                if (auto p = itemPath(item); !p.empty()) result.push_back(p);
                item->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return result;
}
} // namespace

bool nativeDialogsAvailable() { return true; }

bool startDialog(DialogKind kind, const std::string& startFolder, const std::string& title, const std::string& tag) {
    auto& s = state();
    bool expected = false;
    if (!s.running.compare_exchange_strong(expected, true)) return false; // one dialog at a time
    HWND owner = static_cast<HWND>(s.owner);
    std::thread([kind, startFolder, title, tag, owner] {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::vector<std::string> paths;
        if (SUCCEEDED(init)) {
            paths = runDialog(kind, startFolder, title, owner);
            CoUninitialize();
        }
        auto& st = state();
        {
            std::lock_guard l(st.mutex);
            st.tag = tag;
            st.paths = std::move(paths);
            st.ready = true;
        }
        st.running.store(false);
    }).detach();
    return true;
}
#else
bool nativeDialogsAvailable() { return false; }
bool startDialog(DialogKind, const std::string&, const std::string&, const std::string&) { return false; }
#endif

} // namespace roy::gui
