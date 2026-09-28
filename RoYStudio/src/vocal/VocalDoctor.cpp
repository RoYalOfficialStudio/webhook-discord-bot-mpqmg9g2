#include "vocal/VocalDoctor.h"
#include "core/Math.h"
#include "dsp/Analysis.h"
#include "vocal/PitchAnalysis.h"
#include "vocal/PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace roy::vocal {

json DoctorReport::toJson() const {
    json j;
    j["measurements"] = {{"durationSec", m.durationSec}, {"peakDb", m.peakDb}, {"rmsDb", m.rmsDb}, {"crestDb", m.crestDb},
                         {"clippedSamples", m.clippedSamples}, {"clipEvents", m.clipEvents}, {"dcOffset", m.dcOffset},
                         {"noiseFloorDb", m.noiseFloorDb}, {"activeLevelDb", m.activeLevelDb}, {"snrDb", m.snrDb},
                         {"silenceRatio", m.silenceRatio}, {"rumbleRatioDb", m.rumbleRatioDb},
                         {"sibilanceRatioDb", m.sibilanceRatioDb}, {"sibilantEvents", m.sibilantEvents},
                         {"plosiveEvents", m.plosiveEvents}, {"breaths", m.breaths.size()}, {"resonancesHz", m.resonancesHz},
                         {"harshnessDb", m.harshnessDb}, {"dynamicRangeDb", m.dynamicRangeDb},
                         {"pitchMedianMidi", m.pitchMedianMidi}, {"inTuneRatio", m.inTuneRatio}, {"offKeyNotes", m.offKeyNotes}};
    j["issues"] = json::array();
    for (auto& i : issues) {
        json fx = json::array();
        for (auto& f : i.fixes) fx.push_back({{"id", f.id}, {"description", f.description}, {"command", f.command}, {"args", f.args}});
        j["issues"].push_back({{"id", i.id}, {"severity", i.severity}, {"title", i.title}, {"detail", i.detail}, {"fixes", fx}});
    }
    return j;
}

DoctorReport examineVocal(const std::vector<std::vector<float>>& audio, double sr, const DoctorSettings& settings) {
    DoctorReport r;
    auto& m = r.m;
    if (audio.empty() || audio[0].empty()) return r;
    const std::vector<float> x = dsp::mixToMono(audio);
    const int64_t n = static_cast<int64_t>(x.size());
    m.durationSec = static_cast<double>(n) / sr;

    // ---- level, clipping, DC ----
    double sum = 0, sumSq = 0;
    float pk = 0;
    for (auto& ch : audio) {
        uint32_t run = 0;
        for (float v : ch) {
            const float a = std::fabs(v);
            pk = std::max(pk, a);
            if (a >= 0.999f) {
                ++m.clippedSamples;
                if (++run == 3) ++m.clipEvents; // 3+ consecutive full-scale samples = clip event
            } else {
                run = 0;
            }
        }
    }
    for (float v : x) {
        sum += v;
        sumSq += static_cast<double>(v) * v;
    }
    m.peakDb = gainToDb(static_cast<double>(pk));
    m.rmsDb = gainToDb(std::sqrt(sumSq / static_cast<double>(n)));
    m.crestDb = m.peakDb - m.rmsDb;
    m.dcOffset = sum / static_cast<double>(n);

    // ---- frame levels, noise floor, silence, dynamics ----
    auto fl = dsp::frameLevels(x.data(), n, sr, 0.05, 0.025);
    m.noiseFloorDb = dsp::percentile(fl.rmsDb, 0.10);
    std::vector<float> active;
    int silent = 0;
    for (float v : fl.rmsDb) {
        if (v < -60.0f) ++silent;
        if (v > m.noiseFloorDb + 10.0 && v > -60.0f) active.push_back(v);
    }
    m.silenceRatio = fl.rmsDb.empty() ? 0 : static_cast<double>(silent) / static_cast<double>(fl.rmsDb.size());
    m.activeLevelDb = active.empty() ? m.rmsDb : dsp::percentile(active, 0.5);
    m.snrDb = m.activeLevelDb - m.noiseFloorDb;
    m.dynamicRangeDb = active.size() > 4 ? dsp::percentile(active, 0.95) - dsp::percentile(active, 0.10) : 0.0;

    // ---- spectral bands (framewise, 2048/1024) ----
    const int fft = 2048, hop = 1024;
    auto eAll = dsp::bandEnergy(x.data(), n, sr, 20, sr / 2, fft, hop);
    auto eLow = dsp::bandEnergy(x.data(), n, sr, 20, 80, fft, hop);
    auto ePlo = dsp::bandEnergy(x.data(), n, sr, 20, 150, fft, hop);
    auto eSib = dsp::bandEnergy(x.data(), n, sr, 5000, std::min(10000.0, sr / 2), fft, hop);
    auto eMid = dsp::bandEnergy(x.data(), n, sr, 200, 2000, fft, hop);
    auto eHar = dsp::bandEnergy(x.data(), n, sr, 2000, 5000, fft, hop);
    double tAll = 0, tLow = 0, tMid = 0, tHar = 0;
    for (size_t i = 0; i < eAll.size(); ++i) {
        tAll += eAll[i];
        tLow += eLow[i];
        tMid += eMid[i];
        tHar += eHar[i];
    }
    m.rumbleRatioDb = 10 * std::log10(tLow / std::max(1e-20, tAll) + 1e-20);
    m.harshnessDb = 10 * std::log10((tHar + 1e-20) / (tMid + 1e-20));
    // sibilance: loudest frames by HF ratio among active frames
    std::vector<float> sibRatio;
    const double activeThr = dbToGain(std::max(m.noiseFloorDb + 10.0, -60.0));
    for (size_t i = 0; i < eAll.size(); ++i) {
        if (std::sqrt(eAll[i]) < activeThr) continue;
        const float ratio = static_cast<float>(10 * std::log10((eSib[i] + 1e-20) / (eAll[i] + 1e-20)));
        sibRatio.push_back(ratio);
        if (ratio > -6.0f) ++m.sibilantEvents;
    }
    m.sibilanceRatioDb = sibRatio.empty() ? -120.0 : dsp::percentile(sibRatio, 0.95);
    // plosives: sudden low-frequency bursts
    const double hopSec = static_cast<double>(hop) / sr;
    for (size_t i = 2; i < ePlo.size(); ++i) {
        const double now = 10 * std::log10(ePlo[i] + 1e-20), before = 10 * std::log10(std::max(ePlo[i - 1], ePlo[i - 2]) + 1e-20);
        const double share = 10 * std::log10((ePlo[i] + 1e-20) / (eAll[i] + 1e-20));
        if (now > -35.0 && now - before > 12.0 && share > -6.0) {
            ++m.plosiveEvents;
            m.plosiveTimes.push_back(static_cast<double>(i) * hopSec);
        }
    }

    // ---- pitch (also used to find breaths = unvoiced, noisy, moderate level) ----
    PitchTrack track;
    PitchAnalysis pa;
    if (settings.analysePitch) {
        track = detectPitch(x.data(), n, sr);
        pa = analyzePitch(track);
        std::vector<double> voiced;
        for (auto& f : track.frames)
            if (f.voiced) voiced.push_back(f.midi);
        if (!voiced.empty()) {
            std::sort(voiced.begin(), voiced.end());
            m.pitchMedianMidi = voiced[voiced.size() / 2];
            m.pitchLowMidi = voiced[voiced.size() / 20];
            m.pitchHighMidi = voiced[voiced.size() * 19 / 20];
        }
        m.inTuneRatio = pa.inTuneRatio;
        if (settings.keyKnown)
            for (auto& t : tuningIssues(pa, settings.key, 1e9))
                if (t.offKey) ++m.offKeyNotes;
        // breaths: unvoiced frames between -50 and -25 dBFS, lasting 150-900 ms
        const double h = track.hopSeconds();
        double runStart = -1;
        for (size_t i = 0; i <= track.frames.size(); ++i) {
            const bool cand = i < track.frames.size() && !track.frames[i].voiced && track.frames[i].rmsDb > -50.0 &&
                              track.frames[i].rmsDb < -22.0;
            if (cand && runStart < 0) runStart = static_cast<double>(i) * h;
            if (!cand && runStart >= 0) {
                const double end = static_cast<double>(i) * h;
                if (end - runStart >= 0.15 && end - runStart <= 0.9) m.breaths.push_back({runStart, end});
                runStart = -1;
            }
        }
    }

    // ---- resonances: peaks of the long-term spectrum vs 1/3-octave smoothing ----
    auto spec = dsp::averageSpectrumDb(x.data(), n, sr, 4096);
    const double binHz = sr / 4096.0;
    for (size_t b = 1; b + 1 < spec.size(); ++b) {
        const double f = static_cast<double>(b) * binHz;
        if (f < 150 || f > 6000) continue;
        const double lo = f / std::pow(2.0, 1.0 / 6.0), hi = f * std::pow(2.0, 1.0 / 6.0);
        double acc = 0;
        int cnt = 0;
        for (size_t k = static_cast<size_t>(lo / binHz); k <= static_cast<size_t>(hi / binHz) && k < spec.size(); ++k) {
            acc += spec[k];
            ++cnt;
        }
        const double smooth = cnt ? acc / cnt : spec[b];
        if (spec[b] > spec[b - 1] && spec[b] >= spec[b + 1] && spec[b] - smooth > 8.0) {
            if (m.resonancesHz.empty() || f / m.resonancesHz.back() > 1.1) m.resonancesHz.push_back(std::round(f));
        }
    }
    // Harmonic peaks of a steady note are not resonances: drop peaks at multiples of the median pitch.
    if (m.pitchMedianMidi > 0) {
        const double f0 = midiToHz(m.pitchMedianMidi);
        std::erase_if(m.resonancesHz, [&](double f) {
            const double h = f / f0;
            return std::fabs(h - std::round(h)) < 0.06;
        });
    }

    // ---- issues ----
    auto add = [&](DoctorIssue i) { r.issues.push_back(std::move(i)); };
    if (m.clipEvents > 0 || m.clippedSamples > 10) {
        add({"clipping", "problem", "Clipping in the recording",
             std::format("{} clipped samples in {} events (|x| >= 0.999). Clipping in the source cannot be undone by gain; "
                         "re-record with lower input gain if possible.", m.clippedSamples, m.clipEvents),
             {}});
    }
    if (std::fabs(m.dcOffset) > 0.002) {
        add({"dc_offset", "warning", "DC offset", std::format("mean {:.4f} (threshold 0.002)", m.dcOffset),
             {{"dc_hpf", "High-pass at 20 Hz to remove DC", "AddInsert",
               {{"typeId", "roy.eq"}, {"name", "DC Removal"}, {"params", {{"lowCut", 20.0}}}}}}});
    }
    if (m.rumbleRatioDb > -25.0) {
        add({"rumble", "warning", "Low-end rumble", std::format("energy below 80 Hz is {:.1f} dB of total (threshold -25 dB)", m.rumbleRatioDb),
             {{"hpf80", "High-pass filter at 80 Hz", "AddInsert", {{"typeId", "roy.eq"}, {"name", "Rumble Filter"}, {"params", {{"lowCut", 80.0}}}}}}});
    }
    if (m.noiseFloorDb > -60.0 && m.snrDb < 40.0) {
        add({"noise", m.noiseFloorDb > -45.0 ? "problem" : "warning", "Background / room noise",
             std::format("noise floor {:.1f} dBFS, SNR {:.1f} dB", m.noiseFloorDb, m.snrDb),
             {{"gate", "Gate below the vocal", "AddInsert",
               {{"typeId", "roy.gate"}, {"name", "Noise Gate"}, {"params", {{"threshold", std::round(m.noiseFloorDb + 6.0)}, {"range", -18.0}}}}},
              {"cleaner", "Spectral noise reduction", "AddInsert",
               {{"typeId", "roy.noisecleaner"}, {"name", "Noise Cleaner"}, {"params", {{"reduction", 12.0}}}}}}});
    }
    if (m.sibilanceRatioDb > -8.0) {
        add({"sibilance", "warning", "Strong sibilance",
             std::format("5-10 kHz share {:.1f} dB in the loudest sibilant frames, {} sibilant frames", m.sibilanceRatioDb, m.sibilantEvents),
             {{"deesser", "De-esser around 6.5 kHz", "AddInsert",
               {{"typeId", "roy.deesser"}, {"name", "De-Esser"}, {"params", {{"frequency", 6500.0}, {"threshold", -28.0}}}}}}});
    }
    if (m.plosiveEvents > 0) {
        add({"plosives", "warning", "Plosives (pops)", std::format("{} low-frequency bursts", m.plosiveEvents),
             {{"hpf100", "High-pass at 100 Hz", "AddInsert", {{"typeId", "roy.eq"}, {"name", "Plosive Filter"}, {"params", {{"lowCut", 100.0}}}}}}});
    }
    if (!m.breaths.empty()) {
        json pts = json::array();
        for (auto& [a, b] : m.breaths) {
            pts.push_back({{"start", a}, {"end", b}});
        }
        add({"breaths", "info", "Breaths detected", std::format("{} breaths (150-900 ms, unvoiced, -50..-22 dBFS)", m.breaths.size()),
             {{"breath_dip", "Lower breaths by 8 dB with volume automation", "BreathReduce", {{"regions", pts}, {"reductionDb", -8.0}}}}});
    }
    for (double f : m.resonancesHz) {
        add({"resonance", "info", std::format("Resonance near {:.0f} Hz", f), "narrow peak > 8 dB above the 1/3-octave average",
             {{"notch", std::format("Cut 3 dB at {:.0f} Hz (Q 5)", f), "AddInsert",
               {{"typeId", "roy.eq"}, {"name", "Resonance Cut"}, {"params", {{"band2Freq", f}, {"band2Gain", -3.0}, {"band2Q", 5.0}}}}}}});
    }
    if (m.harshnessDb > 3.0) {
        add({"harshness", "warning", "Harsh upper mids", std::format("2-5 kHz is {:.1f} dB above 200 Hz-2 kHz (threshold +3 dB)", m.harshnessDb),
             {{"harsh_cut", "Wide cut at 3.5 kHz", "AddInsert",
               {{"typeId", "roy.eq"}, {"name", "Harshness"}, {"params", {{"band3Freq", 3500.0}, {"band3Gain", -2.5}, {"band3Q", 1.0}}}}}}});
    }
    if (m.dynamicRangeDb > 20.0) {
        add({"dynamics", "info", "Wide dynamic range", std::format("{:.1f} dB between quiet and loud phrases", m.dynamicRangeDb),
             {{"comp", "Compressor 3:1", "AddInsert",
               {{"typeId", "roy.compressor"}, {"name", "Vocal Comp"}, {"params", {{"threshold", std::round(m.activeLevelDb)}, {"ratio", 3.0}}}}}}});
    }
    if (m.peakDb < -18.0 && m.peakDb > -100.0) {
        add({"level", "info", "Low recording level", std::format("peak {:.1f} dBFS", m.peakDb),
             {{"normalize", "Normalize clip to -3 dBFS (clip gain, non-destructive)", "NormalizeClip", {{"targetDb", -3.0}}}}});
    }
    if (settings.analysePitch && !pa.notes.empty() && (m.inTuneRatio < 0.7 || m.offKeyNotes > 0)) {
        add({"pitch", "info", "Pitch deviations",
             std::format("{:.0f} % of stable notes within +-25 cents{}", m.inTuneRatio * 100.0,
                         settings.keyKnown ? std::format(", {} notes outside {}", m.offKeyNotes, settings.key.name()) : ""),
             {{"guardian", "Pitch Guardian ASSIST (original stays available)", "PitchGuardian", {{"mode", "assist"}}}}});
    }
    return r;
}

} // namespace roy::vocal
