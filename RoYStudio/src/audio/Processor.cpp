#include "audio/Processor.h"

#include <algorithm>
#include <cmath>

namespace roy {

Processor::Processor(std::vector<ParamInfo> params) : info_(std::move(params)) {
    values_ = std::make_unique<std::atomic<float>[]>(info_.size());
    for (size_t i = 0; i < info_.size(); ++i) values_[i].store(info_[i].defaultValue);
}

int Processor::findParam(const std::string& id) const {
    for (size_t i = 0; i < info_.size(); ++i)
        if (info_[i].id == id) return static_cast<int>(i);
    return -1;
}

float Processor::getParam(const std::string& id) const {
    const int i = findParam(id);
    return i >= 0 ? getParam(i) : 0.0f;
}

void Processor::setParam(int i, float v) noexcept {
    if (i < 0 || i >= numParams()) return;
    const auto& inf = info_[static_cast<size_t>(i)];
    if (!std::isfinite(v)) v = inf.defaultValue;
    v = std::clamp(v, std::min(inf.minValue, inf.maxValue), std::max(inf.minValue, inf.maxValue));
    if (inf.steps > 1) v = std::round(v);
    values_[static_cast<size_t>(i)].store(v, std::memory_order_relaxed);
}

bool Processor::setParam(const std::string& id, float v) {
    const int i = findParam(id);
    if (i < 0) return false;
    setParam(i, v);
    return true;
}

json Processor::saveState() const {
    json j = json::object();
    json params = json::object();
    for (int i = 0; i < numParams(); ++i) params[info_[static_cast<size_t>(i)].id] = getParam(i);
    j["params"] = params;
    return j;
}

void Processor::loadState(const json& state) {
    if (!state.is_object()) return;
    auto it = state.find("params");
    if (it == state.end() || !it->is_object()) return;
    for (auto& [k, v] : it->items())
        if (v.is_number()) setParam(k, v.get<float>());
}

ProcessorFactory& ProcessorFactory::instance() {
    static ProcessorFactory f;
    return f;
}

void ProcessorFactory::add(const std::string& typeId, const std::string& displayName, bool instrument, Creator c) {
    entries_[typeId] = Entry{typeId, displayName, instrument, std::move(c)};
}

std::unique_ptr<Processor> ProcessorFactory::create(const std::string& typeId) const {
    auto it = entries_.find(typeId);
    if (it != entries_.end()) return it->second.create();
    for (auto& [prefix, creator] : prefixes_)
        if (typeId.rfind(prefix, 0) == 0) return creator(typeId);
    return nullptr;
}

bool ProcessorFactory::has(const std::string& typeId) const {
    if (entries_.count(typeId)) return true;
    for (auto& [prefix, c] : prefixes_)
        if (typeId.rfind(prefix, 0) == 0) return true;
    return false;
}

std::vector<ProcessorFactory::Entry> ProcessorFactory::entries() const {
    std::vector<Entry> out;
    for (auto& [k, e] : entries_) out.push_back(e);
    return out;
}

void ProcessorFactory::addPrefix(const std::string& prefix, PrefixCreator c) { prefixes_[prefix] = std::move(c); }

} // namespace roy
