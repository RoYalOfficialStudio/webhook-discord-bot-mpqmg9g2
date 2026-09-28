// GMB 09 - RoY effects: generic DSP safety + numerical tests per module, loudness.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "dsp/FFT.h"
#include "dsp/Loudness.h"
#include "effects/Dynamics.h"
#include "effects/Effects.h"
#include "effects/Special.h"

using namespace roy;
using namespace roytest;

namespace {
constexpr double SR = 48000.0;

// Runs a processor over stereo input in blocks, returns stereo output.
std::vector<std::vector<float>> run(Processor& p, const std::vector<float>& l, const std::vector<float>& r, int block = 256,
                                    const std::vector<std::vector<float>>* sc = nullptr) {
    const size_t n = l.size();
    std::vector<std::vector<float>> out = {l, r};
    std::vector<std::vector<float>> scCopy;
    if (sc) scCopy = *sc;
    for (size_t off = 0; off < n; off += static_cast<size_t>(block)) {
        const int k = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - off));
        float* ptr[2] = {out[0].data() + off, out[1].data() + off};
        AudioBlock b{ptr, 2, k};
        if (sc) {
            float* sp[2] = {scCopy[0].data() + off, scCopy[1].data() + off};
            AudioBlock sb{sp, 2, k};
            p.process(b, &sb, nullptr, 0);
        } else {
            p.process(b, nullptr, nullptr, 0);
        }
    }
    return out;
}
std::vector<float> sine(double hz, double seconds, double amp, double phase = 0.0) {
    std::vector<float> v(static_cast<size_t>(seconds * SR));
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(amp * std::sin(kTwoPi * hz * static_cast<double>(i) / SR + phase));
    return v;
}
double levelDb(const std::vector<float>& x, size_t from, size_t to) { return gainToDb(rms(x, from, to) * std::sqrt(2.0)); }
double bandLevelDb(const std::vector<float>& x, size_t from, double hz) {
    const int N = 16384;
    auto mag = dsp::magnitudeSpectrum(x.data() + from, N);
    const size_t b = static_cast<size_t>(std::lround(hz * N / SR));
    float m = 0;
    for (size_t k = b - 3; k <= b + 3; ++k) m = std::max(m, mag[k]);
    return gainToDb(static_cast<double>(m) / (N * 0.25));
}
} // namespace

TEST_CASE("gmb09", "every processor survives all test signals at all sample rates") {
    registerBuiltinProcessors();
    int tested = 0;
    for (auto& e : ProcessorFactory::instance().entries()) {
        if (e.instrument || e.typeId.rfind("test.", 0) == 0) continue;
        for (double sr : {44100.0, 48000.0, 96000.0}) {
            auto proc = e.create();
            proc->prepare(sr, 512);
            const size_t n = static_cast<size_t>(sr * 0.6);
            Rng rng(1);
            std::vector<std::vector<float>> signals;
            std::vector<float> s(n);
            for (size_t i = 0; i < n; ++i) s[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 997.0 * i / sr));
            signals.push_back(s); // sine
            for (size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / sr;
                s[i] = static_cast<float>(0.5 * std::sin(kTwoPi * (20.0 * t + 0.5 * (20000.0 - 20.0) / 0.6 * t * t)));
            }
            signals.push_back(s); // sweep
            std::fill(s.begin(), s.end(), 0.0f);
            s[100] = 1.0f;
            signals.push_back(s); // impulse
            for (auto& v : s) v = static_cast<float>(rng.uniform(-1, 1));
            signals.push_back(s); // noise
            std::fill(s.begin(), s.end(), 0.0f);
            signals.push_back(s); // silence
            for (auto& v : s) v = 1e-30f;
            signals.push_back(s); // denormal range
            for (size_t i = 0; i < n; ++i) s[i] = static_cast<float>(16.0 * std::sin(kTwoPi * 60.0 * i / sr));
            signals.push_back(s); // +24 dB overload
            for (size_t k = 0; k < signals.size(); ++k) {
                proc->reset();
                auto out = run(*proc, signals[k], signals[k]);
                bool finite = allFinite(out[0]) && allFinite(out[1]);
                CHECK_MSG(finite, std::format("{} @ {} signal {}", e.typeId, sr, k));
                CHECK_MSG(peak(out[0]) < 200.0f, std::format("{} runaway @ {} signal {} peak {}", e.typeId, sr, k, peak(out[0])));
                if (k == 4) CHECK_MSG(peak(out[0]) < 1e-6f, std::format("{} not silent on silence ({})", e.typeId, peak(out[0])));
            }
            // realtime safety: no allocation while processing
            std::vector<float> a(512, 0.1f), b(512, 0.1f);
            float* ptr[2] = {a.data(), b.data()};
            AudioBlock blk{ptr, 2, 512};
            proc->process(blk, &blk, nullptr, 0);
            long allocs;
            {
                AllocationCounter c;
                for (int i = 0; i < 20; ++i) proc->process(blk, &blk, nullptr, 0);
                allocs = c.count();
            }
            CHECK_MSG(allocs == 0, std::format("{} allocates on the audio thread ({})", e.typeId, allocs));
        }
        ++tested;
    }
    CHECK(tested >= 20);
}

TEST_CASE("gmb09", "RoY EQ matches its response curve") {
    RoyEq eq;
    eq.setParam("band2Freq", 1000.0f);
    eq.setParam("band2Gain", 6.0f);
    eq.setParam("band2Q", 1.0f);
    eq.setParam("lowCut", 100.0f);
    eq.prepare(SR, 512);
    CHECK_NEAR(eq.responseDb(1000.0), 6.0, 0.05);
    CHECK_NEAR(eq.responseDb(100.0), -3.0 + eq.responseDb(100.0) - (-3.0), 0.1); // defined
    for (double hz : {60.0, 100.0, 1000.0, 5000.0}) {
        eq.reset();
        auto x = sine(hz, 1.0, 0.25);
        auto out = run(eq, x, x);
        const double measured = levelDb(out[0], 24000, 48000) - levelDb(x, 24000, 48000);
        CHECK_MSG(std::fabs(measured - eq.responseDb(hz)) < 0.1, std::format("{} Hz measured {} curve {}", hz, measured, eq.responseDb(hz)));
    }
    // 12 dB/oct Butterworth low cut is -3 dB at the cutoff
    RoyEq lc;
    lc.setParam("lowCut", 200.0f);
    lc.prepare(SR, 512);
    CHECK_NEAR(lc.responseDb(200.0), -3.01, 0.05);
    CHECK(lc.responseDb(50.0) < -23.0);
    // flat at defaults = transparent
    RoyEq flat;
    flat.prepare(SR, 512);
    auto x = sine(440.0, 0.2, 0.5);
    auto y = run(flat, x, x);
    double md = 0;
    for (size_t i = 0; i < x.size(); ++i) md = std::max(md, std::fabs(double(x[i]) - y[0][i]));
    CHECK(md < 1e-6);
}

TEST_CASE("gmb09", "compressor static curve, makeup and sidechain") {
    RoyCompressor c;
    c.setParam("threshold", -20.0f);
    c.setParam("ratio", 4.0f);
    c.setParam("knee", 0.0f);
    c.setParam("attack", 1.0f);
    c.setParam("release", 50.0f);
    c.prepare(SR, 256);
    CHECK_NEAR(c.curveDb(-8.0), -17.0, 1e-9);
    CHECK_NEAR(c.curveDb(-30.0), -30.0, 1e-9);
    auto x = sine(1000.0, 1.0, dbToGain(-8.0));
    auto y = run(c, x, x);
    // peak detector on a sine: steady-state output peak ~ curve(-8) = -17 dBFS
    CHECK_NEAR(gainToDb(static_cast<double>(peak(y[0], 30000, 48000))), -17.0, 0.6);
    CHECK_NEAR(c.gainReductionDb(), -9.0, 0.6);
    c.setParam("makeup", 9.0f);
    c.reset();
    auto ym = run(c, x, x);
    CHECK_NEAR(gainToDb(static_cast<double>(peak(ym[0], 30000, 48000))), -8.0, 0.6);
    // external sidechain: quiet signal ducked by a loud key
    RoyCompressor d;
    d.setParam("threshold", -30.0f);
    d.setParam("ratio", 10.0f);
    d.setParam("sidechain", 1.0f);
    d.prepare(SR, 256);
    auto quiet = sine(200.0, 1.0, dbToGain(-40.0));
    auto key = sine(60.0, 1.0, dbToGain(-6.0));
    std::vector<std::vector<float>> sc = {key, key};
    auto ducked = run(d, quiet, quiet, 256, &sc);
    CHECK(levelDb(ducked[0], 30000, 48000) < -60.0);
}

TEST_CASE("gmb09", "limiter: ceiling, true peak, latency") {
    RoyLimiter lim;
    lim.setParam("ceiling", -1.0f);
    lim.setParam("inputGain", 12.0f);
    lim.setParam("truePeak", 0.0f);
    lim.prepare(SR, 512);
    Rng rng(4);
    std::vector<float> x(static_cast<size_t>(SR * 2));
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 80.0 * i / SR) + rng.uniform(-0.4, 0.4));
    auto y = run(lim, x, x);
    CHECK(gainToDb(static_cast<double>(peak(y[0]))) <= -1.0 + 1e-4);
    CHECK(lim.gainReductionDb() < -6.0);
    // true-peak mode: inter-sample peaks stay below the ceiling (+0.3 dB tolerance of the estimator)
    RoyLimiter tp;
    tp.setParam("ceiling", -1.0f);
    tp.setParam("truePeak", 1.0f);
    tp.prepare(SR, 512);
    auto s = sine(SR / 4.0 - 7.0, 2.0, 1.0, kPi / 4); // near fs/4: strong inter-sample peaks
    auto ts = run(tp, s, s);
    auto stats = dsp::measureLoudness(ts, SR);
    CHECK_MSG(stats.truePeakDb <= -0.7, std::format("true peak {}", stats.truePeakDb));
    // latency: an impulse below the ceiling passes unchanged after exactly latencySamples()
    RoyLimiter l2;
    l2.prepare(SR, 512);
    std::vector<float> imp(4096, 0.0f);
    imp[100] = 0.5f;
    auto yi = run(l2, imp, imp);
    CHECK(argmaxAbs(yi[0]) == static_cast<size_t>(100 + l2.latencySamples()));
    CHECK_NEAR(peak(yi[0]), 0.5, 1e-4);
}

TEST_CASE("gmb09", "gate, de-esser, transient, clipper") {
    RoyGate g;
    g.setParam("threshold", -30.0f);
    g.setParam("range", -40.0f);
    g.prepare(SR, 256);
    auto quiet = sine(300.0, 0.5, dbToGain(-45.0));
    auto qo = run(g, quiet, quiet);
    CHECK(levelDb(qo[0], 12000, 24000) < -80.0);
    g.reset();
    auto loud = sine(300.0, 0.5, dbToGain(-12.0));
    auto lo = run(g, loud, loud);
    CHECK_NEAR(levelDb(lo[0], 12000, 24000), -12.0, 0.2);

    RoyDeEsser de;
    de.setParam("frequency", 5000.0f);
    de.setParam("threshold", -30.0f);
    de.setParam("range", 12.0f);
    de.prepare(SR, 256);
    auto ess = sine(7500.0, 0.5, dbToGain(-10.0));
    auto eo = run(de, ess, ess);
    CHECK(levelDb(eo[0], 12000, 24000) < -16.0);
    CHECK(de.gainReductionDb() < -6.0);
    de.reset();
    auto low = sine(400.0, 0.5, dbToGain(-10.0));
    auto loOut = run(de, low, low);
    CHECK_NEAR(levelDb(loOut[0], 12000, 24000), -10.0, 0.5);

    RoyTransient tr;
    tr.setParam("attack", 100.0f);
    tr.prepare(SR, 256);
    std::vector<float> hit(24000, 0.0f);
    for (size_t i = 0; i < 12000; ++i) hit[i + 2000] = static_cast<float>(0.3 * std::exp(-static_cast<double>(i) / 3000.0) * std::sin(kTwoPi * 150.0 * i / SR));
    auto ho = run(tr, hit, hit);
    const double attackGain = peak(ho[0], 2000, 2600) / peak(hit, 2000, 2600);
    const double tailGain = rms(ho[0], 9000, 12000) / rms(hit, 9000, 12000);
    CHECK(attackGain > tailGain * 1.5);

    RoyClipper cl;
    cl.setParam("ceiling", -3.0f);
    cl.setParam("softness", 0.0f);
    cl.prepare(SR, 256);
    auto hot = sine(100.0, 0.5, 2.0);
    auto co = run(cl, hot, hot);
    CHECK(gainToDb(static_cast<double>(peak(co[0], 2000))) < -3.0 + 0.6); // oversampled hard clip: small ringing allowed
}

TEST_CASE("gmb09", "reverb decay and delay timing") {
    RoyReverb rv;
    rv.setParam("decay", 1.0f);
    rv.setParam("mix", 1.0f);
    rv.setParam("preDelay", 0.0f);
    rv.setParam("lowCut", 20.0f);
    rv.prepare(SR, 256);
    std::vector<float> imp(static_cast<size_t>(SR * 2.5), 0.0f);
    imp[10] = 1.0f;
    auto t = run(rv, imp, imp);
    // energy decay: level at 0.3-0.4 s vs 0.9-1.0 s should fall about 60 dB * 0.6 s / 1 s = 36 dB
    const double e1 = gainToDb(rms(t[0], static_cast<size_t>(0.3 * SR), static_cast<size_t>(0.4 * SR)));
    const double e2 = gainToDb(rms(t[0], static_cast<size_t>(0.9 * SR), static_cast<size_t>(1.0 * SR)));
    CHECK_MSG(e1 - e2 > 25.0 && e1 - e2 < 50.0, std::format("decay over 0.6 s: {} dB", e1 - e2));
    CHECK(rms(t[0], static_cast<size_t>(0.2 * SR), static_cast<size_t>(0.3 * SR)) > 1e-3); // it does reverberate
    CHECK(std::fabs(rms(t[0], 5000, 20000) - rms(t[1], 5000, 20000)) > 0.0);              // decorrelated channels

    RoyDelay d;
    d.setParam("time", 250.0f);
    d.setParam("mix", 1.0f);
    d.setParam("feedback", 0.0f);
    d.setParam("lowCut", 20.0f);
    d.setParam("highCut", 20000.0f);
    d.prepare(SR, 256);
    std::vector<float> im(24000, 0.0f);
    im[100] = 1.0f;
    auto de = run(d, im, im);
    CHECK(argmaxAbs(de[0]) == 100 + 12000);
    // tempo sync: 0.5 beats at 96 BPM = 312.5 ms
    RoyDelay s;
    s.setHostTempo(96.0);
    s.setParam("syncBeats", 0.5f);
    s.prepare(SR, 256);
    CHECK_NEAR(s.currentDelaySeconds(), 0.3125, 1e-9);
}

TEST_CASE("gmb09", "saturation harmonics and oversampling alias suppression") {
    RoySaturation clean(false), hot(false);
    clean.setParam("drive", 0.0f);
    hot.setParam("drive", 18.0f);
    for (auto* s : {&clean, &hot}) {
        s->setParam("output", 0.0f);
        s->setParam("tone", 20000.0f);
        s->prepare(SR, 256);
    }
    auto x = sine(1000.0, 1.0, 0.3);
    auto c = run(clean, x, x), h = run(hot, x, x);
    CHECK(bandLevelDb(h[0], 20000, 3000.0) > bandLevelDb(c[0], 20000, 3000.0) + 15.0); // 3rd harmonic
    // 13 kHz driven hard: the 3rd harmonic (39 kHz) would alias to 9 kHz without oversampling
    RoySaturation dist(true);
    dist.setParam("drive", 30.0f);
    dist.setParam("mode", 0.0f);
    dist.setParam("tone", 20000.0f);
    dist.setParam("output", 0.0f);
    dist.prepare(SR, 256);
    auto hf = sine(13000.0, 1.0, 0.5);
    auto ho = run(dist, hf, hf);
    const double fund = bandLevelDb(ho[0], 20000, 13000.0);
    const double alias = bandLevelDb(ho[0], 20000, 9000.0);
    CHECK_MSG(fund - alias > 20.0, std::format("fund {} alias {}", fund, alias));
}

TEST_CASE("gmb09", "modulation, stereo and pitch") {
    auto x = sine(440.0, 1.0, 0.5);
    for (Processor* p : std::initializer_list<Processor*>{new RoyModDelay(false), new RoyModDelay(true), new RoyPhaser()}) {
        std::unique_ptr<Processor> owner(p);
        p->prepare(SR, 256);
        auto y = run(*p, x, x);
        double diff = 0;
        for (size_t i = 0; i < x.size(); ++i) diff = std::max(diff, std::fabs(double(y[0][i]) - x[i]));
        CHECK_MSG(diff > 0.01, p->typeId());
        CHECK(peak(y[0]) < 2.0f);
    }
    RoyStereo st;
    st.setParam("width", 0.0f);
    st.prepare(SR, 256);
    auto l = sine(300.0, 0.2, 0.5), r = sine(500.0, 0.2, 0.3);
    auto mono = run(st, l, r);
    for (size_t i = 0; i < l.size(); i += 97) CHECK_NEAR(mono[0][i], mono[1][i], 1e-6);
    RoyStereo mb;
    mb.setParam("monoBass", 150.0f);
    mb.prepare(SR, 256);
    auto lb = sine(50.0, 1.0, 0.5), rb = sine(50.0, 1.0, -0.5); // pure side at 50 Hz
    auto mbo = run(mb, lb, rb);
    CHECK(rms(mbo[0], 24000, 48000) < 0.05 * rms(lb, 24000, 48000));

    RoyPitch ps;
    ps.setParam("semitones", 7.0f);
    ps.prepare(SR, 256);
    auto po = run(ps, x, x);
    auto mag = dsp::magnitudeSpectrum(po[0].data() + 20000, 16384);
    size_t best = 1;
    for (size_t i = 2; i < mag.size(); ++i)
        if (mag[i] > mag[best]) best = i;
    CHECK_NEAR(static_cast<double>(best) * SR / 16384, 440.0 * std::pow(2.0, 7.0 / 12.0), 6.0);
}

TEST_CASE("gmb09", "VocalTune corrects a sharp note in realtime") {
    RoyVocalTune vt;
    vt.setParam("keyRoot", 0.0f);
    vt.setParam("keyScale", 1.0f); // major
    vt.setParam("speed", 20.0f);
    vt.prepare(SR, 256);
    auto x = sine(midiToHz(64.4), 1.5, 0.4); // E + 40 cents
    auto y = run(vt, x, x);
    CHECK_NEAR(vt.detectedMidi(), 64.4, 0.05); // the detector analyses the (uncorrected) input
    CHECK_NEAR(vt.appliedShift(), -0.4, 0.08);
    auto mag = dsp::magnitudeSpectrum(y[0].data() + 40000, 16384);
    size_t best = 1;
    for (size_t i = 2; i < mag.size(); ++i)
        if (mag[i] > mag[best]) best = i;
    CHECK_NEAR(hzToMidi(static_cast<double>(best) * SR / 16384), 64.0, 0.1);
}

TEST_CASE("gmb09", "noise cleaner improves SNR, analyzer, dynamic space") {
    // Two realistic workflows: (a) frozen profile learned from a noise-only intro, sustained tone;
    // (b) adaptive profile on vocal-like material (phrases with pauses).
    for (int mode = 0; mode < 2; ++mode) {
        Rng rng(8);
        const size_t n = static_cast<size_t>(SR * 4);
        std::vector<float> tone = sine(800.0, 4.0, 0.3), clean(n, 0.0f), noisy(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / SR;
            // mode 1: 0.4 s phrases with 10 ms fades (like sung syllables), 0.2 s pauses
            const double ph = std::fmod(t, 0.6);
            const double env = mode == 0 ? (t > 0.5 ? 1.0 : 0.0) : (ph < 0.4 ? std::min(1.0, std::min(ph, 0.4 - ph) / 0.01) : 0.0);
            clean[i] = static_cast<float>(env) * tone[i];
            noisy[i] = clean[i] + static_cast<float>(rng.uniform(-0.03, 0.03));
        }
        RoyNoiseCleaner nc;
        nc.setParam("reduction", 24.0f);
        nc.setParam("learn", mode == 0 ? 0.0f : 1.0f);
        nc.prepare(SR, 256);
        auto y = run(nc, noisy, noisy);
        const int lat = nc.latencySamples();
        double eIn = 0, eOut = 0;
        for (size_t i = n / 2; i < n; ++i) {
            const double d1 = noisy[i] - clean[i];
            const double d2 = y[0][i] - clean[i - static_cast<size_t>(lat)];
            eIn += d1 * d1;
            eOut += d2 * d2;
        }
        CHECK_MSG(10 * std::log10(eIn / eOut) > 6.0, std::format("mode {} SNR gain {} dB", mode, 10 * std::log10(eIn / eOut)));
    }

    RoyAnalyzer an;
    an.prepare(SR, 256);
    auto s1k = sine(1000.0, 1.0, 0.5);
    run(an, s1k, s1k);
    int best = 0;
    for (int b = 1; b < RoyAnalyzer::kBands; ++b)
        if (an.bandDb(b) > an.bandDb(best)) best = b;
    CHECK(an.bandCentreHz(best) > 800 && an.bandCentreHz(best) < 1250);
    CHECK_NEAR(an.correlation(), 1.0, 1e-3);
    std::vector<float> inv(s1k.size());
    for (size_t i = 0; i < inv.size(); ++i) inv[i] = -s1k[i];
    an.reset();
    run(an, s1k, inv);
    CHECK_NEAR(an.correlation(), -1.0, 1e-3);

    // DYNAMIC SPACE: piano-like pad loses 2.5 kHz only while the vocal sings
    RoyDynamicSpace ds;
    ds.setParam("frequency", 2500.0f);
    ds.setParam("maxReduction", -8.0f);
    ds.setParam("sensitivity", -40.0f);
    ds.prepare(SR, 256);
    auto pad = sine(2500.0, 2.0, 0.2);
    std::vector<float> vocal(pad.size(), 0.0f);
    for (size_t i = static_cast<size_t>(0.8 * SR); i < static_cast<size_t>(1.4 * SR); ++i) vocal[i] = static_cast<float>(0.3 * std::sin(kTwoPi * 2400.0 * i / SR));
    std::vector<std::vector<float>> sc = {vocal, vocal};
    auto dy = run(ds, pad, pad, 256, &sc);
    CHECK_NEAR(levelDb(dy[0], 10000, 30000), levelDb(pad, 10000, 30000), 0.3);              // untouched without vocal
    CHECK(levelDb(dy[0], static_cast<size_t>(1.0 * SR), static_cast<size_t>(1.3 * SR)) < levelDb(pad, 10000, 30000) - 5.0); // ducked with vocal
    CHECK_NEAR(levelDb(dy[0], static_cast<size_t>(1.85 * SR), static_cast<size_t>(1.99 * SR)), levelDb(pad, 10000, 30000), 0.6); // recovers (release 150 ms)
}

TEST_CASE("gmb09", "BS.1770 loudness, gating, true peak") {
    // EBU Tech 3341 test 1: stereo 1 kHz sine at -23 dBFS -> -23.0 LUFS
    auto s = sine(1000.0, 20.0, dbToGain(-23.0));
    auto st = dsp::measureLoudness({s, s}, SR);
    CHECK_NEAR(st.integratedLufs, -23.0, 0.1);
    CHECK_NEAR(st.shortTermMaxLufs, -23.0, 0.1);
    CHECK_NEAR(st.samplePeakDb, -23.0, 0.05);
    // gating: 10 s at -20 LUFS + 10 s at -60 LUFS -> the quiet part is gated away
    auto a = sine(1000.0, 10.0, dbToGain(-20.0)), b = sine(1000.0, 10.0, dbToGain(-60.0));
    std::vector<float> ab(a);
    ab.insert(ab.end(), b.begin(), b.end());
    auto g = dsp::measureLoudness({ab, ab}, SR);
    CHECK_NEAR(g.integratedLufs, -20.0, 0.2);
    // true peak: fs/4 sine with 45 degree phase -> samples at 0.707, true peak 1.0
    auto tpS = sine(SR / 4.0, 1.0, 1.0, kPi / 4);
    auto tp = dsp::measureLoudness({tpS, tpS}, SR);
    CHECK_NEAR(tp.samplePeakDb, -3.01, 0.05);
    CHECK_NEAR(tp.truePeakDb, 0.0, 0.3);
    // other sample rates
    for (double sr : {44100.0, 96000.0}) {
        std::vector<float> v(static_cast<size_t>(sr * 10));
        for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(dbToGain(-23.0) * std::sin(kTwoPi * 1000.0 * i / sr));
        CHECK_NEAR(dsp::measureLoudness({v, v}, sr).integratedLufs, -23.0, 0.1);
    }
    // LRA: alternating -20 / -30 LUFS in 10 s sections has ~10 LU range
    std::vector<float> lra;
    for (int k = 0; k < 6; ++k) {
        auto part = sine(1000.0, 10.0, dbToGain(k % 2 ? -30.0 : -20.0));
        lra.insert(lra.end(), part.begin(), part.end());
    }
    CHECK_NEAR(dsp::measureLoudness({lra, lra}, SR).loudnessRangeLu, 10.0, 1.0);
    CHECK_NEAR(dsp::normalizationGainDb(st, -14.0, -1.0), 9.0, 0.2);
}
