#pragma once
// Native dialogs of the operating system (Windows: IFileOpenDialog in folder mode).
#include <string>
#include <vector>

namespace roy::gui {

// Lets the user pick a folder. Returns "" if cancelled or not available (non-Windows builds use
// the text field instead).
std::string pickFolder(const std::string& startFolder, const std::string& title);
bool nativeFolderPickerAvailable();
// Lets the user pick one or more audio files (MP3, WAV, FLAC, OGG, AIFF). Returns {} if cancelled
// or not available (non-Windows builds: use the BROWSER or type the path).
std::vector<std::string> pickAudioFiles(const std::string& startFolder, const std::string& title);

} // namespace roy::gui
