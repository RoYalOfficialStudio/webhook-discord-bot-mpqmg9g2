// STEMS: IStemSeparator with BasicStemSeparator (DSP) and AdvancedStemSeparator (adapter for an
// external engine). The external engine here is a MOCK (tests/tools/roy_test_stem_engine) that
// only writes fixed fractions of the input - it tests the adapter, not separation quality.
#include "TestFramework.h"
#include "TestHelpers.h"

#include "core/Files.h"
#include "stems/StemSeparator.h"

#include <fstream>

using namespace roy;
using namespace roytest;
namespace fs = std::filesystem;

namespace {
constexpr double SR = 48000.0;

stems::Channels testMix() {
    auto d = makeSine(SR, 330.0, 1.0, 0.4f);
    return d->channels;
}

fs::path writeConfig(const std::string& name, const std::string& mode, int timeoutSec = 60, bool withOther = false) {
    const fs::path dir = tempDir("stems_cfg_" + name);
    nlohmann::json j = {{"command", ROY_TEST_STEM_ENGINE},
                        {"args", {"{in}", "{out}", mode}},
                        {"stems", {{"Vocals", "{out}/vocals.wav"}, {"Drums", "{out}/drums.wav"}}},
                        {"timeoutSec", timeoutSec},
                        {"label", "MOCK engine"}};
    if (withOther) j["stems"]["Other"] = "{out}/vocals.wav";
    std::ofstream(dir / "stem_engine.json") << j.dump(2);
    return dir / "stem_engine.json";
}
} // namespace

TEST_CASE("stems", "IStemSeparator registry: basic always available, advanced reports why not") {
    stems::registerBuiltinSeparators();
    auto* basic = stems::StemRegistry::instance().find("roy.dsp-basic");
    auto* adv = stems::StemRegistry::instance().find("roy.external");
    REQUIRE(basic != nullptr);
    REQUIRE(adv != nullptr);
    CHECK(basic->available());
    stems::AdvancedStemSeparator none(tempDir("stems_none") / "stem_engine.json");
    std::string why;
    CHECK(!none.available(&why));
    CHECK(why.find("no separation engine configured") != std::string::npos);
    auto r = none.separate(testMix(), SR);
    CHECK(r.stems.empty());
    CHECK(!r.quality.warnings.empty());
    // broken config / missing executable are reported, never guessed
    const fs::path dir = tempDir("stems_bad");
    std::ofstream(dir / "a.json") << "{ not json";
    CHECK(!stems::AdvancedStemSeparator(dir / "a.json").available(&why));
    std::ofstream(dir / "b.json") << R"({"command": "/definitely/not/here", "stems": {"Vocals": "{out}/v.wav"}})";
    CHECK(!stems::AdvancedStemSeparator(dir / "b.json").available(&why));
    CHECK(why.find("not found") != std::string::npos);
    std::ofstream(dir / "c.json") << R"({"command": "x"})";
    CHECK(!stems::AdvancedStemSeparator(dir / "c.json").available(&why));
}

TEST_CASE("stems", "advanced adapter: runs the engine, reads its stems, residual goes to Other, sum == input") {
    const auto mix = testMix();
    stems::AdvancedStemSeparator adv(writeConfig("ok", "ok"));
    std::string why;
    REQUIRE_MSG_OK(adv.available(&why), why);
    CHECK(adv.description().find("MOCK engine") != std::string::npos);
    auto r = adv.separate(mix, SR);
    REQUIRE(r.names.size() == 3);
    CHECK(r.names[2] == "Other");
    REQUIRE(r.find("Vocals") != nullptr);
    CHECK_NEAR((*r.find("Vocals"))[0][1000], 0.5f * mix[0][1000], 1e-6);
    CHECK_NEAR((*r.find("Other"))[0][1000], 0.25f * mix[0][1000], 1e-6);
    CHECK(r.quality.reconstructionErrorDb < -100.0);
    CHECK(r.quality.method.find("MOCK") != std::string::npos);
    // an engine that already provides "Other" gets the residual added to it
    stems::AdvancedStemSeparator adv2(writeConfig("other", "ok", 60, true));
    auto r2 = adv2.separate(mix, SR);
    REQUIRE(r2.names.size() == 3);
    CHECK(r2.quality.reconstructionErrorDb < -100.0);
}

TEST_CASE("stems", "advanced adapter failures: exit code, hang (timeout), missing output, wrong rate, cancel") {
    const auto mix = testMix();
    auto fails = [&](const std::string& mode, const std::string& expect, int timeout = 60) {
        stems::AdvancedStemSeparator a(writeConfig(mode, mode, timeout));
        auto r = a.separate(mix, SR);
        bool found = false;
        for (auto& w : r.quality.warnings) found |= w.find(expect) != std::string::npos;
        CHECK_MSG(r.stems.empty() && found, mode + ": " + (r.quality.warnings.empty() ? "" : r.quality.warnings[0]));
    };
    fails("fail", "exit code 3");
    fails("missing", "output missing");
    fails("rate", "44100");
    const auto t0 = std::chrono::steady_clock::now();
    fails("hang", "did not finish", 10);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 30.0);
    stems::AdvancedStemSeparator h(writeConfig("cancel", "hang", 600));
    auto r = h.separate(mix, SR, [](double) { return false; });
    CHECK(r.stems.empty());
}

TEST_CASE("stems", "basic separator keeps its behaviour behind the interface") {
    stems::BasicStemSeparator b;
    const auto mix = testMix();
    auto r = b.separate(mix, SR);
    REQUIRE(r.stems.size() == 4);
    CHECK(r.quality.reconstructionErrorDb < -60.0);
    CHECK(!r.quality.warnings.empty());
    stems::IStemSeparator& i = b;
    CHECK(i.id() == "roy.dsp-basic");
}
