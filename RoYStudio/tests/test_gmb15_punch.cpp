// GMB 15 - punch in/out + pre-roll command (the recorder's punch itself: test_gmb04_record "punch in and out").
#include "TestFramework.h"
#include "TestHelpers.h"

#include "commands/Commands.h"
#include "project/ProjectIO.h"

using namespace roy;
using namespace roytest;

TEST_CASE("gmb15", "SetPunch: range check, pre-roll in bars, one undo step, saved with the project") {
    Project p = makeNewProject("punch", 48000.0, 120.0);
    UndoManager undo{p};
    CommandRegistry reg;
    registerCoreCommands(reg);
    CommandContext ctx{p, undo, nullptr};
    CHECK(!reg.execute(ctx, "SetPunch", {{"enabled", true}, {"inBeat", 8.0}, {"outBeat", 8.0}}));
    CHECK(!reg.execute(ctx, "SetPunch", {{"enabled", true}, {"inBeat", -1.0}, {"outBeat", 4.0}}));
    REQUIRE(reg.execute(ctx, "SetPunch", {{"enabled", true}, {"inBeat", 8.0}, {"outBeat", 12.0}, {"preRollBars", 1}}));
    CHECK(p.settings.punchEnabled);
    CHECK(p.settings.punchInBeat == 8.0);
    CHECK(p.settings.punchOutBeat == 12.0);
    CHECK(p.settings.preRollBeats == 4.0); // 1 bar of 4/4
    Project q;
    REQUIRE(deserializeProject(serializeProject(p), q));
    CHECK(q.settings.punchEnabled);
    CHECK(q.settings.punchOutBeat == 12.0);
    CHECK(q.settings.preRollBeats == 4.0);
    REQUIRE(reg.execute(ctx, "SetPunch", {{"enabled", false}}));
    CHECK(!p.settings.punchEnabled);
    CHECK(p.settings.punchInBeat == 8.0); // the range is kept for the next time
    undo.undo();
    CHECK(p.settings.punchEnabled);
    undo.undo();
    CHECK(!p.settings.punchEnabled);
}
