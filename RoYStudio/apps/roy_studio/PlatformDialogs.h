#pragma once
// Native dialogs of the operating system (Windows: IFileOpenDialog in folder mode).
#include <string>

namespace roy::gui {

// Lets the user pick a folder. Returns "" if cancelled or not available (non-Windows builds use
// the text field instead).
std::string pickFolder(const std::string& startFolder, const std::string& title);
bool nativeFolderPickerAvailable();

} // namespace roy::gui
