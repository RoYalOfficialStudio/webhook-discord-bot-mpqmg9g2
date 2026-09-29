#include "PlatformDialogs.h"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace roy::gui {

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
} // namespace

bool nativeFolderPickerAvailable() { return true; }

std::string pickFolder(const std::string& startFolder, const std::string& title) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        const std::wstring t = widen(title);
        dlg->SetTitle(t.c_str());
        IShellItem* start = nullptr;
        if (!startFolder.empty() && SUCCEEDED(SHCreateItemFromParsingName(widen(startFolder).c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}
std::vector<std::string> pickAudioFiles(const std::string& startFolder, const std::string& title) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::vector<std::string> result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
        const COMDLG_FILTERSPEC types[] = {{L"Audio (MP3, WAV, FLAC, OGG, AIFF)", L"*.mp3;*.wav;*.flac;*.ogg;*.aif;*.aiff"},
                                           {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, types);
        const std::wstring t = widen(title);
        dlg->SetTitle(t.c_str());
        IShellItem* start = nullptr;
        if (!startFolder.empty() && SUCCEEDED(SHCreateItemFromParsingName(widen(startFolder).c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dlg->GetResults(&items))) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    if (FAILED(items->GetItemAt(i, &item))) continue;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        result.push_back(narrow(path));
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
                items->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}
#else
bool nativeFolderPickerAvailable() { return false; }
std::string pickFolder(const std::string&, const std::string&) { return {}; }
std::vector<std::string> pickAudioFiles(const std::string&, const std::string&) { return {}; }
#endif

} // namespace roy::gui
