#include "project/Automation.h"
#include "project/AutomationShape.h"

#include <algorithm>

namespace roy::automation {

std::optional<double> laneValueAt(const AutomationLane& lane, double beat) {
    if (lane.points.empty()) return std::nullopt;
    auto pts = lane.points;
    std::stable_sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a.beat < b.beat; });
    if (beat <= pts.front().beat) return pts.front().value;
    if (beat >= pts.back().beat) return pts.back().value;
    auto it = std::upper_bound(pts.begin(), pts.end(), beat, [](double b, const AutomationPoint& p) { return b < p.beat; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const double span = b.beat - a.beat;
    const double x = span > 0 ? (beat - a.beat) / span : 0.0;
    return interpolate(a.value, b.value, a.curve, a.tension, x);
}

const AutomationLane* tempoLane(const Project& p) {
    const MixerChannel* m = p.master();
    for (auto& l : p.automation)
        if (l.enabled && l.paramId == "tempo" && l.slotId.empty() && (!m || l.channelId == m->id) && !l.points.empty()) return &l;
    return nullptr;
}

bool applyTempoAutomation(Project& p, double res) {
    const AutomationLane* lane = tempoLane(p);
    if (!lane) return false;
    res = std::clamp(res, 1.0 / 64.0, 1.0);
    auto pts = lane->points;
    std::stable_sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a.beat < b.beat; });
    std::vector<TempoEvent> ev;
    ev.push_back({0.0, pts.front().value});
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& a = pts[i];
        ev.push_back({a.beat, a.value});
        if (i + 1 == pts.size() || a.curve == Hold) continue;
        const auto& b = pts[i + 1];
        const double span = b.beat - a.beat;
        if (span <= 0) continue;
        const int steps = std::max(1, static_cast<int>(std::ceil(span / res)));
        for (int k = 0; k < steps; ++k) {
            const double x0 = static_cast<double>(k) / steps, xm = (k + 0.5) / steps;
            ev.push_back({a.beat + span * x0, interpolate(a.value, b.value, a.curve, a.tension, xm)});
        }
    }
    const auto before = p.tempo.tempoEvents();
    p.tempo.setTempoEvents(std::move(ev));
    const auto& after = p.tempo.tempoEvents();
    if (before.size() != after.size()) return true;
    for (size_t i = 0; i < before.size(); ++i)
        if (before[i].beat != after[i].beat || before[i].bpm != after[i].bpm) return true;
    return false;
}

} // namespace roy::automation
