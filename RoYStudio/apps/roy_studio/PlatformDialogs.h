#pragma once
// Native dialogs of the operating system (Windows: IFileOpenDialog for folders and audio files).
//
// They run on their OWN thread in a single-threaded COM apartment and never block the UI: the
// audio driver (WASAPI via miniaudio) puts the main thread into the multi-threaded COM apartment,
// where the Windows file dialogs hang or crash. Start a dialog, keep drawing frames, and pick up
// the result with takeDialogResult().
#include <string>
#include <vector>

namespace roy::gui {

enum class DialogKind { Folder, AudioFiles };

// False on non-Windows builds (use the BROWSER or type the path instead).
bool nativeDialogsAvailable();
inline bool nativeFolderPickerAvailable() { return nativeDialogsAvailable(); }
// The main window (HWND) that owns the dialogs, so they open on top of it.
void setDialogOwner(void* nativeWindow);
// Opens the dialog. `tag` comes back with the result. False if not available or one is already open.
bool startDialog(DialogKind kind, const std::string& startFolder, const std::string& title, const std::string& tag);
bool dialogRunning();
// True once when a dialog closed: `paths` holds the chosen folder / files (empty = cancelled).
bool takeDialogResult(std::string& tag, std::vector<std::string>& paths);

} // namespace roy::gui
