// SETUP WIZARD (first start), SYSTEM CHECK window and FIRST REAL MUSIC SESSION guide.
#include "MusicSession.h"
#include "PlatformDialogs.h"
#include "Ui.h"

#include "core/Files.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::gui {

namespace {
ImVec4 statusColor(const std::string& s) {
    if (s.rfind("PASS", 0) == 0) return ImGui::ColorConvertU32ToFloat4(col::Green);
    if (s.rfind("WARNING", 0) == 0) return ImGui::ColorConvertU32ToFloat4(col::Orange);
    if (s.rfind("FAIL", 0) == 0) return ImGui::ColorConvertU32ToFloat4(col::Red);
    return ImGui::ColorConvertU32ToFloat4(col::IvoryDim);
}

// Horizontal input meter (-60..0 dBFS, both input channels) + a text verdict. Returns the peak.
float inputMeter(App& app, float width) {
    const float dpi = ImGui::GetFontSize() / 15.0f;
    static float shownL = 0, shownR = 0;
    shownL = std::max(app.engine().inputPeak(0), shownL * 0.9f);
    shownR = std::max(app.engine().inputPeak(1), shownR * 0.9f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = 8 * dpi;
    for (int c = 0; c < 2; ++c) {
        const float v = c == 0 ? shownL : shownR;
        const float x = std::clamp((20.0f * std::log10(std::max(1e-6f, v)) + 60.0f) / 60.0f, 0.0f, 1.0f);
        const ImVec2 a(p0.x, p0.y + c * (h + 2)), b(p0.x + width, a.y + h);
        dl->AddRectFilled(a, b, col::rgb(0x2A2A30));
        dl->AddRectFilled(a, ImVec2(a.x + width * x, b.y), x > 59.0f / 60.0f ? col::Red : x > 48.0f / 60.0f ? col::Orange : col::Green);
    }
    for (int dbMark : {-48, -24, -12, -6}) {
        const float x = p0.x + width * (dbMark + 60) / 60.0f;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + 2 * h + 2), col::rgb(0x000000, 160));
    }
    ImGui::Dummy(ImVec2(width, 2 * h + 2));
    const float peak = std::max(shownL, shownR);
    const float db = 20.0f * std::log10(std::max(1e-6f, peak));
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(db > -1.0f ? col::Red : db > -60.0f ? col::Green : col::IvoryDim), "%s",
                       db > -1.0f ? "TOO LOUD - lower the gain" : db > -60.0f ? std::format("{:.0f} dBFS", db).c_str() : "no signal");
    return peak;
}

void deviceList(App& app, bool input) {
    auto cfg = app.audioConfig();
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::BeginChild(input ? "inlist" : "outlist", ImVec2(560 * dpi, 150 * dpi), ImGuiChildFlags_Borders);
    if (input && ImGui::Selectable("Off (playback only)", !cfg.enableInput)) {
        cfg.enableInput = false;
        app.changeAudio(cfg);
    }
    const std::string& cur = input ? cfg.inputDevice : cfg.outputDevice;
    const bool on = !input || cfg.enableInput;
    if (ImGui::Selectable("Windows default device", on && cur.empty())) {
        (input ? cfg.inputDevice : cfg.outputDevice).clear();
        cfg.enableInput = input ? true : cfg.enableInput;
        app.changeAudio(cfg);
    }
    for (auto& d : input ? app.device().inputDevices() : app.device().outputDevices())
        if (ImGui::Selectable((d.name + (d.isDefault ? "   (default)" : "")).c_str(), on && cur == d.name)) {
            (input ? cfg.inputDevice : cfg.outputDevice) = d.name;
            if (input) cfg.enableInput = true;
            app.changeAudio(cfg);
        }
    ImGui::EndChild();
}
} // namespace

// =============================================================================== SETUP WIZARD
void drawSetupWizard(App& app) {
    if (!app.showFirstRun) return;
    if (!ImGui::IsPopupOpen("WELCOME TO ROY STUDIO")) ImGui::OpenPopup("WELCOME TO ROY STUDIO");
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(640 * dpi, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("WELCOME TO ROY STUDIO", nullptr, ImGuiWindowFlags_NoResize)) return;
    static const char* titles[] = {"WELCOME", "STEP 1 - OUTPUT DEVICE", "STEP 2 - INPUT DEVICE", "STEP 3 - BUFFER", "STEP 4 - SAMPLE RATE",
                                   "STEP 5 - MIDI", "SYSTEM CHECK"};
    int& step = app.setupStep;
    step = std::clamp(step, 0, 6);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Gold), "%s", titles[step]);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60 * dpi);
    ImGui::TextDisabled("%d / 6", step);
    ImGui::Separator();
    auto cfg = app.audioConfig();
    ImGui::TextDisabled("%s", app.audioStatus().c_str());

    switch (step) {
    case 0:
        ImGui::TextWrapped("Welcome! This check takes about 2 minutes: speakers, microphone, buffer, sample rate and MIDI. "
                           "Everything can be changed later (Audio menu). Nothing is installed or changed outside RoY's own folder%s.",
                           files::portableMode() ? " (PORTABLE mode: settings, logs and projects stay in the RoY folder)" : "");
        ImGui::TextDisabled("This is an UNSIGNED development/test build.");
        break;
    case 1:
        ImGui::TextWrapped("Choose where you want to hear RoY (speakers, headset or audio interface):");
        deviceList(app, false);
        if (goldButton("PLAY TEST TONE")) app.playTestTone();
        ImGui::SameLine();
        ImGui::TextDisabled("you should hear a 1-second beep (440 Hz)");
        if (app.device().backendName() == "WASAPI") {
            bool ex = cfg.exclusive;
            if (ImGui::Checkbox("WASAPI Exclusive (lower latency; other programs are muted while RoY runs)", &ex)) {
                cfg.exclusive = ex;
                app.changeAudio(cfg);
            }
            ImGui::TextDisabled("Default: WASAPI Shared - works with every device, no extra driver needed.");
        }
        ImGui::TextDisabled("ASIO is not part of this build (no extra driver needed; WASAPI is used).");
        break;
    case 2: {
        ImGui::TextWrapped("Choose your microphone or audio interface input:");
        deviceList(app, true);
        ImGui::Text("LIVE INPUT LEVEL");
        inputMeter(app, 360 * dpi);
        if (app.micTestState() == 1) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Red), "RECORDING 3 s - speak now...");
        } else if (goldButton("TEST MICROPHONE")) {
            app.testMicrophone();
        }
        if (app.micTestState() == 2) {
            ImGui::SameLine();
            const float db = 20.0f * std::log10(std::max(1e-6f, app.micTestPeak()));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app.micTestPeak() > 0.001f ? col::Green : col::Red), "%s",
                               app.micTestPeak() > 0.001f ? std::format("signal OK (peak {:.0f} dBFS) - you hear it played back", db).c_str()
                                                          : "NO SIGNAL");
        }
        ImGui::TextWrapped("No signal? Check the cable / gain knob, and Windows: Settings > Privacy & security > Microphone > "
                           "'Let desktop apps access your microphone' = On.");
        break;
    }
    case 3: {
        ImGui::TextWrapped("Buffer = how much audio is prepared at once. Small: less delay when recording. Large: safer with many tracks/plugins.");
        const int sizes[] = {64, 128, 256, 512, 1024};
        for (int b : sizes) {
            ImGui::SameLine(b == 64 ? 0.0f : -1.0f);
            if (b == 64) ImGui::NewLine();
            if (toggleButton(std::to_string(b).c_str(), cfg.bufferSize == b, col::Gold, ImVec2(70 * dpi, 0)) && cfg.bufferSize != b) {
                cfg.bufferSize = b;
                app.changeAudio(cfg);
            }
        }
        ImGui::TextDisabled("Recommended start: 256 samples (%.1f ms at %.0f Hz)", 256000.0 / std::max(1.0, cfg.sampleRate), cfg.sampleRate);
        if (goldButton("PLAY TEST TONE##buf")) app.playTestTone();
        const uint64_t drop = app.overloadsSinceAudioChange();
        if (drop > 0)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "WARNING: %llu dropouts with %d samples - choose a larger buffer (256 or 512).",
                               static_cast<unsigned long long>(drop), cfg.bufferSize);
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Green), "no dropouts measured with this buffer");
        break;
    }
    case 4: {
        std::vector<double> native;
        const auto rates = app.offeredSampleRates(&native);
        ImGui::TextWrapped("48000 Hz is the standard for music production. Only supported rates are offered.");
        for (double sr : rates) {
            const bool isNative = std::find(native.begin(), native.end(), sr) != native.end();
            if (ImGui::RadioButton(std::format("{:.0f} Hz{}", sr, isNative ? "   (native for this device)" : "").c_str(), cfg.sampleRate == sr) && cfg.sampleRate != sr) {
                cfg.sampleRate = sr;
                app.changeAudio(cfg);
            }
        }
        if (!cfg.exclusive) ImGui::TextDisabled("Shared mode: Windows converts other rates for the device automatically.");
        break;
    }
    case 5: {
        ImGui::TextWrapped("MIDI keyboard or pad controller (optional):");
        if (auto* mi = app.midiInput()) {
            const auto devs = mi->devices();
            if (devs.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::IvoryDim), "No MIDI device found - that is fine: press SKIP. "
                                                                                                 "A keyboard plugged in later is detected automatically.");
            for (auto& d : devs) ImGui::BulletText("%s %s", d.name.c_str(), mi->isOpen(d.id) ? "(ready)" : "(switched off)");
            ImGui::Text("Press a key: %s", mi->lastMessageText().c_str());
        }
        break;
    }
    case 6: {
        if (app.lastSystemCheck().empty()) app.runSystemCheck();
        if (ImGui::BeginTable("check", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (auto& c : app.lastSystemCheck()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(c.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(statusColor(c.status), "%s", c.status.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", c.detail.c_str());
            }
            ImGui::EndTable();
        }
        const std::string all = support::overall(app.lastSystemCheck());
        ImGui::TextColored(statusColor(all), "RESULT: %s", all.c_str());
        if (ImGui::Button("Run again")) app.runSystemCheck();
        break;
    }
    }

    ImGui::Separator();
    if (step > 0 && ImGui::Button("< Back")) --step;
    ImGui::SameLine();
    if (step == 0) {
        if (goldButton("START SETUP")) step = 1;
        ImGui::SameLine();
        if (ImGui::Button("Later")) { // shown again on the next start
            app.showFirstRun = false;
            ImGui::CloseCurrentPopup();
        }
    } else if (step < 6) {
        if (goldButton(step == 5 ? "NEXT (system check)" : "NEXT >")) {
            ++step;
            if (step == 6) app.runSystemCheck();
        }
        if (step == 5) {
            ImGui::SameLine();
            if (ImGui::Button("SKIP")) {
                step = 6;
                app.runSystemCheck();
            }
        }
    } else {
        if (goldButton("FINISH + START FIRST REAL MUSIC SESSION")) {
            app.finishFirstRun();
            app.showMusicSession = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("FINISH")) {
            app.finishFirstRun();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

// =============================================================================== SYSTEM CHECK
void drawSystemCheck(App& app) {
    if (!app.showSystemCheck) return;
    const float dpi = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextWindowSize(ImVec2(720 * dpi, 460 * dpi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SYSTEM CHECK", &app.showSystemCheck)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("syscheck", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthFixed, 190 * dpi);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 80 * dpi);
        ImGui::TableSetupColumn("Details");
        ImGui::TableHeadersRow();
        for (auto& c : app.lastSystemCheck()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(statusColor(c.status), "%s", c.status.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", c.detail.c_str());
        }
        ImGui::EndTable();
    }
    const std::string all = support::overall(app.lastSystemCheck());
    ImGui::TextColored(statusColor(all), "OVERALL: %s", all.c_str());
    if (ImGui::Button("Run again")) app.runSystemCheck();
    ImGui::SameLine();
    if (ImGui::Button("CREATE DIAGNOSTIC PACKAGE")) app.createDiagnosticPackage();
    ImGui::End();
}

// =============================================================================== MUSIC SESSION
void drawMusicSession(App& app) {
    if (!app.showMusicSession) return;
    static SessionState st;
    const float dpi = ImGui::GetFontSize() / 15.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 520 * dpi, vp->WorkPos.y + 90 * dpi), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500 * dpi, vp->WorkSize.y - 140 * dpi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("FIRST REAL MUSIC SESSION", &app.showMusicSession)) {
        ImGui::End();
        return;
    }
    const auto& steps = musicSessionSteps();
    int passed = 0;
    size_t current = steps.size();
    for (size_t i = 0; i < steps.size(); ++i) {
        const bool ok = st.results.count(i) && st.results[i].rfind("PASS", 0) == 0;
        passed += ok;
        if (!ok && current == steps.size()) current = i;
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Gold), "%d / %zu steps passed", passed, steps.size());
    ImGui::ProgressBar(static_cast<float>(passed) / static_cast<float>(steps.size()), ImVec2(-1, 6 * dpi), "");
    ImGui::TextWrapped("Work from top to bottom. DO IT performs the step with normal (undoable) commands - or do it yourself and press "
                       "\"done by hand\". Results are saved for the diagnostic package.");
    if (ImGui::SmallButton("Open results file folder")) app.openFolder(files::userDataDirectory());
    ImGui::Separator();

    // input level is tracked while part B is open
    const float in = std::max(app.engine().inputPeak(0, false), app.engine().inputPeak(1, false));
    st.maxInput = std::max(st.maxInput, in);

    ImGui::BeginChild("steps");
    std::string group;
    bool groupOpen = false;
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& s = steps[i];
        if (s.group != group) {
            group = s.group;
            bool anyOpen = false;
            for (size_t k = 0; k < steps.size(); ++k)
                if (steps[k].group == group && k == current) anyOpen = true;
            ImGui::SetNextItemOpen(anyOpen, ImGuiCond_Once);
            groupOpen = ImGui::CollapsingHeader(group.c_str());
        }
        if (!groupOpen) continue;
        ImGui::PushID(static_cast<int>(i));
        const std::string res = st.results.count(i) ? st.results[i] : std::string();
        const char* mark = res.rfind("PASS", 0) == 0 ? "[OK]" : res.rfind("FAIL", 0) == 0 ? "[!!]" : i == current ? " >> " : "[  ]";
        ImGui::TextColored(res.empty() ? ImGui::ColorConvertU32ToFloat4(i == current ? col::Gold : col::IvoryDim) : statusColor(res), "%s", mark);
        ImGui::SameLine();
        ImGui::TextUnformatted(std::format("{}. {}", i + 1, s.title).c_str());
        ImGui::Indent(34 * dpi);
        ImGui::TextDisabled("%s", s.hint.c_str());
        // step-specific helpers
        if (s.title == "Choose the input device") {
            auto cfg = app.audioConfig();
            ImGui::SetNextItemWidth(300 * dpi);
            if (ImGui::BeginCombo("Input", !cfg.enableInput ? "Off" : cfg.inputDevice.empty() ? "Windows default" : cfg.inputDevice.c_str())) {
                if (ImGui::Selectable("Windows default", cfg.enableInput && cfg.inputDevice.empty())) {
                    cfg.enableInput = true;
                    cfg.inputDevice.clear();
                    app.changeAudio(cfg);
                }
                for (auto& d : app.device().inputDevices())
                    if (ImGui::Selectable(d.name.c_str(), cfg.enableInput && cfg.inputDevice == d.name)) {
                        cfg.enableInput = true;
                        cfg.inputDevice = d.name;
                        app.changeAudio(cfg);
                    }
                ImGui::EndCombo();
            }
        } else if (s.title == "Input level") {
            ImGui::Text("INPUT");
            ImGui::SameLine();
            inputMeter(app, 260 * dpi);
        } else if (s.title == "Record 10-20 seconds" && app.recording() && st.recordStartSec >= 0) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - st.recordStartSec;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(t >= 10.0 ? col::Green : col::Red), "REC %.0f s", t);
        } else if (s.title == "Choose the export folder") {
            static char folder[1024] = "";
            static std::string shown;
            if (ImGui::IsWindowAppearing() || !folder[0] || (shown != app.exportFolder && !app.exportFolder.empty())) {
                std::snprintf(folder, sizeof(folder), "%s", app.currentExportFolder().string().c_str());
                shown = app.exportFolder;
            }
            ImGui::SetNextItemWidth(300 * dpi);
            if (ImGui::InputText("##exportFolder", folder, sizeof(folder))) shown = app.exportFolder = folder;
            if (nativeDialogsAvailable()) {
                ImGui::SameLine();
                if (ImGui::Button(dialogRunning() ? "(window open)" : "Browse...") && !dialogRunning())
                    startDialog(DialogKind::Folder, app.currentExportFolder().string(), "RoY Studio - export folder", "exportFolder");
            }
        } else if (s.title == "A/B original / corrected" && !st.vocalClip.empty() && app.project().findAudioClip(st.vocalClip)) {
            if (ImGui::SmallButton("A ORIGINAL")) app.run("VocalAB", {{"clipId", st.vocalClip}, {"use", "original"}});
            ImGui::SameLine();
            if (ImGui::SmallButton("B CORRECTED")) app.run("VocalAB", {{"clipId", st.vocalClip}, {"use", "corrected"}});
        } else if (s.title == "Scan the RoY TEST plugins" && app.pluginScanRunning()) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Orange), "scanning...");
        }
        if (goldButton("DO IT")) runSessionStep(app, st, i);
        ImGui::SameLine();
        if (ImGui::SmallButton("done by hand")) {
            st.results[i] = "PASS - done by hand";
            app.message(0, s.title + ": done by hand");
        }
        if (!res.empty()) ImGui::TextColored(statusColor(res), "%s", res.c_str());
        ImGui::Unindent(34 * dpi);
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextWrapped("Finished? Fill in TestKit\\TEST_RESULTS.md and send it together with Help > CREATE DIAGNOSTIC PACKAGE.");
    ImGui::EndChild();
    ImGui::End();
}

} // namespace roy::gui

namespace roy::gui {

// =============================================================================== IMPORT BEAT
// A bought / downloaded beat (MP3, WAV ...): own track from bar 1, song tempo + key can follow it.
void drawImportBeat(App& app) {
    if (!app.showImportBeat) return;
    const float dpi = ImGui::GetFontSize() / 15.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560 * dpi, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin("IMPORT BEAT (MP3 / WAV)", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!open) app.cancelImportBeat();
        return;
    }
    static fs::path shownFile;
    static bool useTempo = false, useKey = false;
    static float bpm = 140.0f;
    static int keyRoot = 9, keyMinor = 1;
    static char pathBuf[1024] = "";
    if (app.importBeatFile.empty()) { // no native file dialog (Linux dev build): type or paste the path
        ImGui::TextWrapped("Path of the beat file (or use the BROWSER: Downloads tab, right-click the file > Import as BEAT):");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##beatpath", pathBuf, sizeof(pathBuf));
        if (goldButton("Analyse") && pathBuf[0]) app.beginImportBeat({fs::path(pathBuf)});
        ImGui::End();
        if (!open) app.cancelImportBeat();
        return;
    }
    const auto* info = app.importBeatInfo();
    if (shownFile != app.importBeatFile) { // new file: defaults are set when its analysis is ready
        shownFile = app.importBeatFile;
        useTempo = useKey = false;
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Gold), "%s", app.importBeatFile.filename().string().c_str());
    static const beatimport::BeatFileInfo* defaultsFor = nullptr;
    if (!info) {
        ImGui::TextDisabled("reading the file, finding tempo and key ...");
        defaultsFor = nullptr;
    } else if (!info->ok) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Red), "cannot read this file: %s", info->error.c_str());
    } else {
        if (defaultsFor != info) { // suggestions: file name first, a confident detection second
            defaultsFor = info;
            const double sb = info->suggestedBpm();
            useTempo = sb > 0;
            bpm = static_cast<float>(sb > 0 ? sb : (info->bpmDetected > 0 ? info->bpmDetected : app.project().tempo.tempoAt(0)));
            const auto sk = info->suggestedKey();
            useKey = sk.has_value();
            const Key k = sk ? *sk : (info->keyRoot >= 0 ? Key{info->keyRoot, info->keyMinor ? ScaleType::NaturalMinor : ScaleType::Major} : app.project().key);
            keyRoot = k.root;
            keyMinor = k.scale == ScaleType::Major ? 0 : 1;
        }
        const int mins = static_cast<int>(info->seconds) / 60;
        ImGui::Text("%d:%02d min  |  %.0f Hz  |  %s", mins, static_cast<int>(info->seconds) % 60, info->sampleRate, info->channels > 1 ? "stereo" : "mono");
        ImGui::SameLine();
        const bool playing = app.previewer().playing() && app.previewer().current() == app.importBeatFile;
        if (toggleButton(playing ? "STOP" : "LISTEN", playing, col::Green)) {
            if (playing) app.previewer().stop();
            else app.previewFile(app.importBeatFile);
        }
        ImGui::Separator();
        // tempo
        ImGui::Checkbox("Set song tempo to", &useTempo);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * dpi);
        ImGui::InputFloat("##bpm", &bpm, 0, 0, "%.1f BPM");
        bpm = std::clamp(bpm, 10.0f, 999.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("x2")) bpm = std::min(999.0f, bpm * 2);
        ImGui::SameLine();
        if (ImGui::SmallButton("/2")) bpm = std::max(10.0f, bpm / 2);
        if (info->bpmFromName) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Green), "   from the file name: %.1f BPM", *info->bpmFromName);
        else if (info->bpmDetected > 0)
            ImGui::TextDisabled("   detected: %.1f BPM (%s) - trap is often double: use x2. Check with the metronome (CLICK).", info->bpmDetected,
                                info->bpmConfidence >= 0.5 ? "fairly sure" : "unsure");
        else ImGui::TextDisabled("   no tempo found - the song keeps %.1f BPM", app.project().tempo.tempoAt(0));
        // key
        ImGui::Checkbox("Set song key to", &useKey);
        ImGui::SameLine();
        static const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        static const char* modes[] = {"Major", "Minor"};
        ImGui::SetNextItemWidth(60 * dpi);
        ImGui::Combo("##root", &keyRoot, roots, 12);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80 * dpi);
        ImGui::Combo("##mode", &keyMinor, modes, 2);
        ImGui::SameLine();
        ImGui::TextDisabled("(LIVE autotune uses it)");
        if (info->keyFromName) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Green), "   from the file name: %s", info->keyFromName->name().c_str());
        else if (info->keyRoot >= 0)
            ImGui::TextDisabled("   detected: %s %s (%s)", roots[info->keyRoot], info->keyMinor ? "Minor" : "Major",
                                info->keyConfidence >= 0.6 ? "fairly sure" : "unsure - the seller's key info is better");
        // headroom: bought beats are mastered loud; with vocals on top the master would clip (crackles)
        static bool headroom = true;
        ImGui::Checkbox("Beat fader -6 dB (room for your vocals - prevents clipping / crackles)", &headroom);
        if (info->peakDb > -0.5)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(info->peakDb > 0.0 ? col::Red : col::Orange), "   the file peaks at %+.1f dBFS%s",
                               info->peakDb, info->peakDb > 0.0 ? " - over 0 dB, it clips without headroom!" : " - very loud master");
        ImGui::Separator();
        ImGui::TextWrapped("The beat goes on its OWN new track, starting at bar 1. RoY copies the file into the project "
                           "(your download stays where it is). One Ctrl+Z undoes everything.");
        if (goldButton("IMPORT BEAT", ImVec2(160 * dpi, 0))) {
            const std::string key = std::string(roots[keyRoot]) + (keyMinor ? " Minor" : " Major");
            if (app.previewer().playing()) app.previewer().stop();
            if (app.importBeat(app.importBeatFile, useTempo ? bpm : 0.0, useKey ? key : std::string(), headroom ? -6.0 : 0.0)) {
                app.showImportBeat = false;
                app.importNextQueued(); // several files picked / dropped: the next one
            }
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel")) open = false;
    ImGui::End();
    if (!open) {
        if (app.previewer().playing()) app.previewer().stop();
        app.cancelImportBeat();
    }
}

} // namespace roy::gui
