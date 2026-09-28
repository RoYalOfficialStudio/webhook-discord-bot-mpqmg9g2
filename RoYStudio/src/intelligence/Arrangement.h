#pragma once
// ENERGY MAP + ARRANGEMENT INTELLIGENCE.
// Technical measurements per bar; high energy is not "better" music.
#include "audio/TempoMap.h"
#include "intelligence/MixIntelligence.h"
#include "project/Project.h"

#include <string>
#include <vector>

namespace roy::arrangei {

struct EnergyFrame {
    double startBeat = 0, endBeat = 0;
    double loudnessLufs = -70;   // short-term loudness of the bar
    double density = 0;          // onsets per second
    double spectralCentroid = 0; // Hz
    double drumActivity = 0;     // 0..1 (share of drum-role energy, or percussive estimate)
    double bassActivity = 0;     // 0..1 (share of energy < 120 Hz)
    double vocalActivity = 0;    // 0..1 (vocal-role tracks active fraction)
    double energy = 0;           // combined, normalised 0..1 over the song (technical)
};

struct EnergyMap {
    std::vector<EnergyFrame> bars;
    std::string note = "Energy is a technical combination of loudness, density and spectrum; higher energy does not mean better music.";
};

EnergyMap buildEnergyMap(const mixi::Channels& master, const std::vector<mixi::TrackAudio>& tracks, double sampleRate,
                         const TempoMap& tempo, double startBeat = 0.0);

struct SectionSuggestion {
    double startBeat = 0, endBeat = 0;
    std::string type;  // intro | verse | pre_hook | hook | bridge | outro
    std::string name;  // "Hook 1"
    int cluster = 0;   // segments with the same cluster repeat
    double meanEnergy = 0;
    double confidence = 0;
};

// Novelty-based segmentation of the energy/timbre features, repetition clustering and labelling.
std::vector<SectionSuggestion> suggestSections(const EnergyMap& map, int minBarsPerSection = 4);

} // namespace roy::arrangei
