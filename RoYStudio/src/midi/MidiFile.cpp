#include "midi/MidiFile.h"
#include "core/Files.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

namespace roy::midi {

namespace fs = std::filesystem;

namespace {
struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    bool ok = true;
    uint8_t u8() {
        if (pos >= b.size()) { ok = false; return 0; }
        return b[pos++];
    }
    uint32_t be(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | u8();
        return v;
    }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t c = u8();
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
        ok = false;
        return v;
    }
};

void putBe(std::vector<uint8_t>& o, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putVlq(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = static_cast<uint8_t>(0x80 | (v & 0x7F));
    while (n--) o.push_back(tmp[n]);
}
} // namespace

bool parseMidi(const std::vector<uint8_t>& bytes, SmfData& out, std::string* error) {
    Reader r{bytes};
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };
    if (bytes.size() < 14 || std::string(bytes.begin(), bytes.begin() + 4) != "MThd") return fail("not a MIDI file");
    r.pos = 4;
    const uint32_t hlen = r.be(4);
    out.format = static_cast<int>(r.be(2));
    const int ntracks = static_cast<int>(r.be(2));
    const uint32_t division = r.be(2);
    if (division & 0x8000) return fail("SMPTE time division is not supported");
    out.ppq = static_cast<int>(division);
    if (out.ppq <= 0) return fail("invalid PPQ");
    r.pos = 8 + hlen;
    struct TempoAt { uint64_t tick; double bpm; };
    std::vector<TempoAt> tempos;
    struct SigAt { uint64_t tick; int num, den; };
    std::vector<SigAt> sigs;
    struct RawNote { uint64_t on, off; int pitch, vel, ch; };
    std::vector<std::pair<std::string, std::vector<RawNote>>> raw;

    for (int t = 0; t < ntracks && r.ok; ++t) {
        if (r.pos + 8 > bytes.size()) break;
        const std::string id(bytes.begin() + static_cast<long>(r.pos), bytes.begin() + static_cast<long>(r.pos) + 4);
        r.pos += 4;
        const uint32_t len = r.be(4);
        const size_t end = std::min(bytes.size(), r.pos + len);
        if (id != "MTrk") {
            r.pos = end;
            continue;
        }
        std::string name;
        std::vector<RawNote> notes;
        std::map<int, std::vector<std::pair<uint64_t, int>>> open; // key ch*128+pitch -> (tick, vel) stack
        uint64_t tick = 0;
        uint8_t running = 0;
        while (r.pos < end && r.ok) {
            tick += r.vlq();
            uint8_t status = r.u8();
            if (status < 0x80) {
                if (!running) return fail("running status without status byte");
                r.pos--;
                status = running;
            }
            if (status == 0xFF) {
                const uint8_t type = r.u8();
                const uint32_t mlen = r.vlq();
                const size_t mstart = r.pos;
                if (type == 0x51 && mlen == 3) {
                    const uint32_t us = r.be(3);
                    if (us > 0) tempos.push_back({tick, 60000000.0 / us});
                } else if (type == 0x58 && mlen >= 2) {
                    const int num = r.u8();
                    const int den = 1 << r.u8();
                    sigs.push_back({tick, num, den});
                } else if (type == 0x03) {
                    name.assign(bytes.begin() + static_cast<long>(mstart), bytes.begin() + static_cast<long>(std::min(end, mstart + mlen)));
                } else if (type == 0x2F) {
                    r.pos = mstart + mlen;
                    break;
                }
                r.pos = mstart + mlen;
                running = 0;
            } else if (status == 0xF0 || status == 0xF7) {
                r.pos += r.vlq();
                running = 0;
            } else {
                running = status;
                const int type = status & 0xF0, ch = status & 0x0F;
                const int d1 = r.u8();
                const int d2 = (type == 0xC0 || type == 0xD0) ? 0 : r.u8();
                const int key = ch * 128 + d1;
                if (type == 0x90 && d2 > 0) {
                    open[key].push_back({tick, d2});
                } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
                    auto& st = open[key];
                    if (!st.empty()) {
                        auto [onTick, vel] = st.front(); // FIFO pairing
                        st.erase(st.begin());
                        notes.push_back({onTick, tick, d1, vel, ch});
                    }
                }
            }
        }
        for (auto& [key, st] : open)
            for (auto& [onTick, vel] : st) notes.push_back({onTick, tick, key % 128, vel, key / 128}); // unterminated
        r.pos = end;
        raw.push_back({name, notes});
    }
    if (!r.ok) return fail("truncated MIDI data");

    // tempo map (ticks -> beats is linear: beat = tick / ppq)
    std::sort(tempos.begin(), tempos.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
    out.tempo = TempoMap(tempos.empty() ? 120.0 : tempos.front().bpm);
    for (size_t i = 1; i < tempos.size(); ++i) out.tempo.addTempoEvent(static_cast<double>(tempos[i].tick) / out.ppq, tempos[i].bpm);
    if (!sigs.empty()) {
        out.tempo.setTimeSignature(sigs.front().num, sigs.front().den);
        for (size_t i = 1; i < sigs.size(); ++i) {
            const double beat = static_cast<double>(sigs[i].tick) / out.ppq;
            out.tempo.addTimeSignature(out.tempo.beatToBarBeat(beat).bar, sigs[i].num, sigs[i].den);
        }
    }
    out.tracks.clear();
    for (auto& [name, notes] : raw) {
        if (notes.empty()) continue;
        SmfTrack t;
        t.name = name;
        for (auto& n : notes) {
            MidiNote m;
            m.pitch = n.pitch;
            m.velocity = std::clamp(n.vel, 1, 127);
            m.channel = n.ch;
            m.startBeat = static_cast<double>(n.on) / out.ppq;
            m.lengthBeats = std::max(1.0 / out.ppq, static_cast<double>(n.off - n.on) / out.ppq);
            t.notes.push_back(m);
        }
        t.channel = t.notes.front().channel;
        std::sort(t.notes.begin(), t.notes.end(), [](auto& a, auto& b) { return a.startBeat < b.startBeat; });
        out.tracks.push_back(std::move(t));
    }
    return true;
}

bool readMidiFile(const fs::path& path, SmfData& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseMidi(b, out, error);
}

std::vector<uint8_t> buildMidi(const SmfData& d) {
    const int ppq = d.ppq > 0 ? d.ppq : 960;
    std::vector<uint8_t> o;
    auto chunk = [&](const char* id, const std::vector<uint8_t>& body) {
        o.insert(o.end(), id, id + 4);
        putBe(o, static_cast<uint32_t>(body.size()), 4);
        o.insert(o.end(), body.begin(), body.end());
    };
    std::vector<uint8_t> hdr;
    putBe(hdr, 1, 2);
    putBe(hdr, static_cast<uint32_t>(d.tracks.size() + 1), 2);
    putBe(hdr, static_cast<uint32_t>(ppq), 2);
    chunk("MThd", hdr);
    auto toTick = [&](double beat) { return static_cast<uint64_t>(std::llround(std::max(0.0, beat) * ppq)); };

    // conductor track: tempo + time signatures
    {
        struct Ev { uint64_t tick; std::vector<uint8_t> bytes; };
        std::vector<Ev> evs;
        for (auto& t : d.tempo.tempoEvents()) {
            const uint32_t us = static_cast<uint32_t>(std::llround(60000000.0 / t.bpm));
            evs.push_back({toTick(t.beat), {0xFF, 0x51, 0x03, static_cast<uint8_t>(us >> 16), static_cast<uint8_t>(us >> 8), static_cast<uint8_t>(us)}});
        }
        for (auto& s : d.tempo.timeSignatures()) {
            int pow2 = 0;
            while ((1 << pow2) < s.denominator) ++pow2;
            evs.push_back({toTick(d.tempo.barToBeat(s.bar)), {0xFF, 0x58, 0x04, static_cast<uint8_t>(s.numerator), static_cast<uint8_t>(pow2), 24, 8}});
        }
        std::stable_sort(evs.begin(), evs.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
        std::vector<uint8_t> body;
        uint64_t last = 0;
        for (auto& e : evs) {
            putVlq(body, static_cast<uint32_t>(e.tick - last));
            last = e.tick;
            body.insert(body.end(), e.bytes.begin(), e.bytes.end());
        }
        body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
        chunk("MTrk", body);
    }
    for (auto& t : d.tracks) {
        struct Ev { uint64_t tick; int order; uint8_t s, d1, d2; };
        std::vector<Ev> evs;
        for (auto& n : t.notes) {
            if (n.muted) continue;
            const uint8_t ch = static_cast<uint8_t>(std::clamp(n.channel, 0, 15));
            evs.push_back({toTick(n.startBeat), 1, static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(n.pitch), static_cast<uint8_t>(std::clamp(n.velocity, 1, 127))});
            evs.push_back({std::max(toTick(n.startBeat) + 1, toTick(n.endBeat())), 0, static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(n.pitch), 0});
        }
        std::stable_sort(evs.begin(), evs.end(), [](auto& a, auto& b) { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });
        std::vector<uint8_t> body;
        if (!t.name.empty()) {
            body.insert(body.end(), {0x00, 0xFF, 0x03});
            putVlq(body, static_cast<uint32_t>(t.name.size()));
            body.insert(body.end(), t.name.begin(), t.name.end());
        }
        uint64_t last = 0;
        for (auto& e : evs) {
            putVlq(body, static_cast<uint32_t>(e.tick - last));
            last = e.tick;
            body.insert(body.end(), {e.s, e.d1, e.d2});
        }
        body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
        chunk("MTrk", body);
    }
    return o;
}

bool writeMidiFile(const fs::path& path, const SmfData& data, bool allowOverwrite, std::string* error) {
    std::error_code ec;
    if (!allowOverwrite && fs::exists(path, ec)) {
        if (error) *error = "file exists: " + path.string();
        return false;
    }
    const auto bytes = buildMidi(data);
    return files::atomicWrite(path, std::string(bytes.begin(), bytes.end()), error);
}

SmfData clipToSmf(const Project& p, const MidiClip& clip, const std::string& name) {
    SmfData d;
    d.tempo = p.tempo;
    SmfTrack t;
    t.name = name;
    const double loop = clip.loopLengthBeats > 0 ? clip.loopLengthBeats : 1e18;
    for (int k = 0; k * loop < clip.lengthBeats && k < 10000; ++k)
        for (auto n : clip.notes) {
            if (clip.loopLengthBeats > 0 && n.startBeat >= loop) continue;
            n.startBeat += k * (clip.loopLengthBeats > 0 ? loop : 0);
            if (n.startBeat >= clip.lengthBeats) continue;
            n.lengthBeats = std::min(n.lengthBeats, clip.lengthBeats - n.startBeat);
            t.notes.push_back(n);
        }
    d.tracks.push_back(t);
    return d;
}

SmfData projectToSmf(const Project& p) {
    SmfData d;
    d.tempo = p.tempo;
    for (auto& tr : p.tracks) {
        if (tr.type != TrackType::Midi) continue;
        SmfTrack t;
        t.name = tr.name;
        for (auto& c : tr.midiClips) {
            if (c.muted) continue;
            auto one = clipToSmf(p, c, tr.name);
            for (auto n : one.tracks[0].notes) {
                n.startBeat += c.startBeat;
                t.notes.push_back(n);
            }
        }
        d.tracks.push_back(t);
    }
    return d;
}

std::vector<std::string> importSmfIntoProject(Project& p, const SmfData& d, double atBeat, bool applyTempo) {
    std::vector<std::string> ids;
    if (applyTempo) p.tempo = d.tempo;
    for (auto& t : d.tracks) {
        const std::string tid = addTrack(p, TrackType::Midi, t.name.empty() ? "MIDI" : t.name).id;
        MidiClip c;
        c.id = files::newId();
        c.name = t.name;
        c.startBeat = atBeat;
        c.notes = t.notes;
        double end = 0;
        for (auto& n : c.notes) end = std::max(end, n.endBeat());
        c.lengthBeats = std::max(1.0, std::ceil(end / 4.0) * 4.0);
        p.findTrack(tid)->midiClips.push_back(c);
        ids.push_back(tid);
    }
    return ids;
}

} // namespace roy::midi
