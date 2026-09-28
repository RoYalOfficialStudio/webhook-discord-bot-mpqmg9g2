#include "export/Mp3Encoder.h"
#include "core/Files.h"
#include "core/Process.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace roy::mp3 {

namespace fs = std::filesystem;

namespace {
// Minimal subset of the lame.h API, resolved at runtime (no link-time dependency).
using gfp_t = void*;
struct Api {
    gfp_t (*init)() = nullptr;
    int (*set_in_samplerate)(gfp_t, int) = nullptr;
    int (*set_out_samplerate)(gfp_t, int) = nullptr;
    int (*set_num_channels)(gfp_t, int) = nullptr;
    int (*set_mode)(gfp_t, int) = nullptr;
    int (*set_brate)(gfp_t, int) = nullptr;
    int (*set_VBR)(gfp_t, int) = nullptr;
    int (*set_VBR_q)(gfp_t, int) = nullptr;
    int (*set_quality)(gfp_t, int) = nullptr;
    int (*set_bWriteVbrTag)(gfp_t, int) = nullptr;
    int (*set_write_id3tag_automatic)(gfp_t, int) = nullptr;
    int (*init_params)(gfp_t) = nullptr;
    int (*encode_float)(gfp_t, const float*, const float*, int, unsigned char*, int) = nullptr;
    int (*flush)(gfp_t, unsigned char*, int) = nullptr;
    size_t (*get_lametag_frame)(gfp_t, unsigned char*, size_t) = nullptr;
    int (*close)(gfp_t) = nullptr;
    const char* (*get_lame_version)() = nullptr;
    void (*id3_init)(gfp_t) = nullptr;
    void (*id3_add_v2)(gfp_t) = nullptr;
    void (*id3_set_title)(gfp_t, const char*) = nullptr;
    void (*id3_set_artist)(gfp_t, const char*) = nullptr;
    void (*id3_set_album)(gfp_t, const char*) = nullptr;
    void (*id3_set_year)(gfp_t, const char*) = nullptr;
    void (*id3_set_comment)(gfp_t, const char*) = nullptr;
    int (*id3_set_track)(gfp_t, const char*) = nullptr;
    int (*id3_set_genre)(gfp_t, const char*) = nullptr;
    size_t (*get_id3v2_tag)(gfp_t, unsigned char*, size_t) = nullptr;
    size_t (*get_id3v1_tag)(gfp_t, unsigned char*, size_t) = nullptr;
};

std::mutex g_mutex;
Api g_api;
bool g_loaded = false;
std::string g_error;

void* openLib(const fs::path& p) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryW(p.wstring().c_str()));
#else
    return dlopen(p.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void* sym(void* lib, const char* n) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), n));
#else
    return dlsym(lib, n);
#endif
}

bool load() {
    if (g_loaded) return true;
    if (!g_error.empty()) return false;
    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("ROY_MP3LAME")) candidates.push_back(env);
    const fs::path exeDir = executableDirectory();
#ifdef _WIN32
    candidates.push_back(exeDir / "roy_mp3lame.dll");
    candidates.push_back("libmp3lame.dll");
#else
    candidates.push_back(exeDir / "roy_mp3lame.so");
    candidates.push_back(exeDir.parent_path() / "roy_mp3lame.so");
    candidates.push_back("libmp3lame.so.0");
#endif
    void* lib = nullptr;
    std::string tried;
    for (auto& c : candidates) {
        tried += (tried.empty() ? "" : ", ") + c.string();
        if ((lib = openLib(c))) break;
    }
    if (!lib) {
        g_error = "LAME MP3 encoder library not found (searched: " + tried + ")";
        return false;
    }
    bool ok = true;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(sym(lib, name));
        if (!fn) {
            ok = false;
            g_error = std::string("LAME library lacks ") + name;
        }
    };
    Api& a = g_api;
    get(a.init, "lame_init");
    get(a.set_in_samplerate, "lame_set_in_samplerate");
    get(a.set_out_samplerate, "lame_set_out_samplerate");
    get(a.set_num_channels, "lame_set_num_channels");
    get(a.set_mode, "lame_set_mode");
    get(a.set_brate, "lame_set_brate");
    get(a.set_VBR, "lame_set_VBR");
    get(a.set_VBR_q, "lame_set_VBR_q");
    get(a.set_quality, "lame_set_quality");
    get(a.set_bWriteVbrTag, "lame_set_bWriteVbrTag");
    get(a.set_write_id3tag_automatic, "lame_set_write_id3tag_automatic");
    get(a.init_params, "lame_init_params");
    get(a.encode_float, "lame_encode_buffer_ieee_float");
    get(a.flush, "lame_encode_flush");
    get(a.get_lametag_frame, "lame_get_lametag_frame");
    get(a.close, "lame_close");
    get(a.get_lame_version, "get_lame_version");
    get(a.id3_init, "id3tag_init");
    get(a.id3_add_v2, "id3tag_add_v2");
    get(a.id3_set_title, "id3tag_set_title");
    get(a.id3_set_artist, "id3tag_set_artist");
    get(a.id3_set_album, "id3tag_set_album");
    get(a.id3_set_year, "id3tag_set_year");
    get(a.id3_set_comment, "id3tag_set_comment");
    get(a.id3_set_track, "id3tag_set_track");
    get(a.id3_set_genre, "id3tag_set_genre");
    get(a.get_id3v2_tag, "lame_get_id3v2_tag");
    get(a.get_id3v1_tag, "lame_get_id3v1_tag");
    g_loaded = ok;
    return ok;
}
} // namespace

bool available(std::string* why) {
    std::lock_guard<std::mutex> lk(g_mutex);
    const bool ok = load();
    if (!ok && why) *why = g_error;
    return ok;
}

std::string encoderVersion() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return load() ? std::string("LAME ") + g_api.get_lame_version() : std::string();
}

bool validBitrate(int k) {
    static const int rates[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    return std::find(std::begin(rates), std::end(rates), k) != std::end(rates);
}

bool supportedSampleRate(int hz) {
    static const int rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    return std::find(std::begin(rates), std::end(rates), hz) != std::end(rates);
}

bool encode(const fs::path& out, const std::vector<std::vector<float>>& ch, int sampleRate, const Options& o, std::string* error) {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!load()) return fail(g_error);
    if (ch.empty() || ch.size() > 2) return fail("MP3 supports mono or stereo only");
    if (ch.size() == 2 && ch[0].size() != ch[1].size()) return fail("channel lengths differ");
    if (!supportedSampleRate(sampleRate)) return fail(std::format("MP3 cannot store {} Hz (use 32, 44.1 or 48 kHz)", sampleRate));
    if (!o.vbr && !validBitrate(o.bitrateKbps)) return fail(std::format("invalid MP3 bitrate {} kbps", o.bitrateKbps));
    std::error_code ec;
    if (fs::exists(out, ec)) return fail("file exists (never overwritten): " + out.string());
    const Api& a = g_api;
    gfp_t g = a.init();
    if (!g) return fail("lame_init failed");
    struct Closer {
        const Api& a;
        gfp_t g;
        ~Closer() { a.close(g); }
    } closer{a, g};
    const int nch = static_cast<int>(ch.size());
    a.set_in_samplerate(g, sampleRate);
    a.set_out_samplerate(g, sampleRate);
    a.set_num_channels(g, nch);
    a.set_mode(g, nch == 1 ? 3 /*MONO*/ : 1 /*JOINT_STEREO*/);
    if (o.vbr) {
        a.set_VBR(g, 4 /*vbr_mtrh = vbr_default*/);
        a.set_VBR_q(g, std::clamp(o.vbrQuality, 0, 9));
    } else {
        a.set_VBR(g, 0 /*vbr_off*/);
        a.set_brate(g, o.bitrateKbps);
    }
    a.set_quality(g, std::clamp(o.quality, 0, 9));
    a.set_bWriteVbrTag(g, 1); // Xing/LAME info frame: exact length + gapless info
    a.set_write_id3tag_automatic(g, 0);
    a.id3_init(g);
    a.id3_add_v2(g);
    if (!o.title.empty()) a.id3_set_title(g, o.title.c_str());
    if (!o.artist.empty()) a.id3_set_artist(g, o.artist.c_str());
    if (!o.album.empty()) a.id3_set_album(g, o.album.c_str());
    if (!o.year.empty()) a.id3_set_year(g, o.year.c_str());
    if (!o.comment.empty()) a.id3_set_comment(g, o.comment.c_str());
    if (!o.track.empty()) a.id3_set_track(g, o.track.c_str());
    if (!o.genre.empty()) a.id3_set_genre(g, o.genre.c_str());
    if (a.init_params(g) < 0) return fail("LAME rejected the encoder settings");

    const fs::path tmp = out.string() + ".partial";
    FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return fail("cannot write " + tmp.string());
    bool ok = true;
    std::vector<unsigned char> buf(1 << 16);
    // ID3v2 at the start
    const size_t v2 = a.get_id3v2_tag(g, buf.data(), buf.size());
    if (v2 > buf.size()) buf.resize(v2), a.get_id3v2_tag(g, buf.data(), buf.size());
    if (v2 > 0) ok &= std::fwrite(buf.data(), 1, v2, f) == v2;
    const long audioStart = std::ftell(f);
    const size_t frames = ch[0].size();
    const size_t block = 4096;
    buf.resize(static_cast<size_t>(1.25 * block + 7200));
    std::vector<float> l(block), r(block);
    for (size_t pos = 0; pos < frames && ok; pos += block) {
        const size_t n = std::min(block, frames - pos);
        for (size_t i = 0; i < n; ++i) {
            l[i] = std::clamp(ch[0][pos + i], -1.0f, 1.0f);
            r[i] = std::clamp(ch[nch > 1 ? 1 : 0][pos + i], -1.0f, 1.0f);
        }
        const int bytes = a.encode_float(g, l.data(), r.data(), static_cast<int>(n), buf.data(), static_cast<int>(buf.size()));
        if (bytes < 0) {
            ok = false;
            if (error) *error = std::format("LAME encode error {}", bytes);
            break;
        }
        ok &= std::fwrite(buf.data(), 1, static_cast<size_t>(bytes), f) == static_cast<size_t>(bytes);
    }
    if (ok) {
        const int bytes = a.flush(g, buf.data(), static_cast<int>(buf.size()));
        ok = bytes >= 0 && std::fwrite(buf.data(), 1, static_cast<size_t>(bytes), f) == static_cast<size_t>(bytes);
    }
    if (ok) { // ID3v1 at the end
        const size_t v1 = a.get_id3v1_tag(g, buf.data(), buf.size());
        if (v1 > 0) ok = std::fwrite(buf.data(), 1, v1, f) == v1;
    }
    if (ok) { // LAME/Xing info frame replaces the first (empty) frame
        const size_t tag = a.get_lametag_frame(g, buf.data(), buf.size());
        if (tag > 0 && tag <= buf.size()) {
            std::fseek(f, audioStart, SEEK_SET);
            ok = std::fwrite(buf.data(), 1, tag, f) == tag;
        }
    }
    ok &= std::fflush(f) == 0;
    ok &= std::fclose(f) == 0;
    if (!ok) {
        fs::remove(tmp, ec);
        return fail(error && !error->empty() ? *error : "writing the MP3 file failed (disk full or no permission?)");
    }
    fs::rename(tmp, out, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("cannot finalize " + out.string());
    }
    return true;
}

} // namespace roy::mp3
