#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace reverie
{
enum Route { routeDelayReverb = 0, routeParallel, routeReverbDelay };

/** Tempo-synced delay lengths, in beats (a quarter note is one beat). */
inline constexpr int numDivisions = 14;
inline constexpr float divisionBeats[numDivisions] =
{
    0.125f, 1.0f / 6.0f, 0.25f, 0.375f, 1.0f / 3.0f, 0.5f, 0.75f,
    2.0f / 3.0f, 1.0f, 1.5f, 4.0f / 3.0f, 2.0f, 3.0f, 4.0f
};
inline constexpr float maxDelayMs = 4000.0f;

namespace format
{
    inline juce::String db (float v, int)       { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
    inline juce::String level (float v, int n)  { return v <= -39.9f ? juce::String ("OFF") : db (v, n); }
    inline juce::String hz (float v, int)       { return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz"
                                                                     : juce::String (v / 1000.0f, 2) + " kHz"; }
    inline juce::String rate (float v, int)     { return juce::String (v, 2) + " Hz"; }
    inline juce::String ms (float v, int)       { return v < 10.0f ? juce::String (v, 2) + " ms"
                                                       : v < 1000.0f ? juce::String (juce::roundToInt (v)) + " ms"
                                                                     : juce::String (v / 1000.0f, 2) + " s"; }
    inline juce::String sec (float v, int n)    { return ms (v * 1000.0f, n); }
    inline juce::String pct (float v, int)      { return juce::String (juce::roundToInt (v)) + " %"; }
    inline juce::String offset (float v, int)   { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; }
    inline juce::String bpm (float v, int)      { return juce::String (v, 1) + " BPM"; }
    inline juce::String lowCut (float v, int n) { return v <= 20.5f ? juce::String ("OFF") : hz (v, n); }
    inline juce::String highCut (float v, int n){ return v >= 19900.0f ? juce::String ("OFF") : hz (v, n); }
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    using Fmt = String (*) (float, int);

    auto addF = [&layout] (const char* id, const char* name, float lo, float hi, float def, Fmt fmt, float centre = 0.0f, float step = 0.0f)
    {
        NormalisableRange<float> range (lo, hi, step);
        if (centre > 0.0f) range.setSkewForCentre (centre);
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def,
                        AudioParameterFloatAttributes().withStringFromValueFunction (fmt)));
    };
    auto addB = [&layout] (const char* id, const char* name, bool def)
    {
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    };
    auto addC = [&layout] (const char* id, const char* name, StringArray choices, int def)
    {
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def));
    };

    const auto arrow = String::fromUTF8 (" \xe2\x80\xba ");
    addC ("route", "Routing", { "Dly" + arrow + "Rev", "Parallel", "Rev" + arrow + "Dly" }, 0);
    addB ("bypass", "Bypass", false);

    // Input: level, and the filters on what is sent into the effects (the dry signal is never filtered)
    addF ("inGain", "Input Gain", -24.0f, 24.0f, 0.0f, format::db);
    addF ("bpm", "Tempo", 40.0f, 240.0f, 120.0f, format::bpm, 0.0f, 0.1f);
    addF ("feedLo", "Feed Low Cut", 20.0f, 1000.0f, 20.0f, format::lowCut, 160.0f);
    addF ("feedHi", "Feed High Cut", 1000.0f, 20000.0f, 20000.0f, format::highCut, 6000.0f);

    // Delay
    addB ("dlyOn", "Delay On", true);
    addC ("dlyType", "Delay Type", { "Digital", "Tape", "Analog", "Lo-Fi", "Reverse" }, 0);
    addC ("dlyMode", "Delay Mode", { "Stereo", "Ping-Pong", "Cross" }, 0);
    addB ("dlySync", "Delay Sync", false);
    addF ("dlyTime", "Delay Time", 1.0f, 2000.0f, 350.0f, format::ms, 300.0f);
    addC ("dlyDiv", "Delay Division", { "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D",
                                        "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D", "1/1" }, 8);
    addF ("dlyOffset", "Delay L/R Offset", -50.0f, 50.0f, 0.0f, format::offset);
    addF ("dlyFb", "Delay Feedback", 0.0f, 110.0f, 35.0f, format::pct);
    addF ("dlyLo", "Delay Low Cut", 20.0f, 1000.0f, 100.0f, format::lowCut, 160.0f);
    addF ("dlyHi", "Delay High Cut", 1000.0f, 20000.0f, 8000.0f, format::highCut, 6000.0f);
    addF ("dlyModRate", "Delay Mod Rate", 0.05f, 8.0f, 0.5f, format::rate, 1.0f);
    addF ("dlyModDepth", "Delay Mod Depth", 0.0f, 100.0f, 0.0f, format::pct);
    addF ("dlyChar", "Delay Character", 0.0f, 100.0f, 40.0f, format::pct);
    addF ("dlyLevel", "Delay Level", -40.0f, 6.0f, -8.0f, format::level);

    // Reverb
    addB ("rvOn", "Reverb On", true);
    addC ("rvType", "Reverb Type", { "Room", "Chamber", "Hall", "Plate", "Spring", "Shimmer" }, 2);
    addF ("rvPre", "Reverb Pre-Delay", 0.0f, 250.0f, 20.0f, format::ms, 60.0f);
    addF ("rvSize", "Reverb Size", 0.0f, 100.0f, 50.0f, format::pct);
    addF ("rvDecay", "Reverb Decay", 0.2f, 30.0f, 2.2f, format::sec, 2.5f);
    addF ("rvDamp", "Reverb Damping", 1000.0f, 20000.0f, 6000.0f, format::hz, 6000.0f);
    addF ("rvMod", "Reverb Mod", 0.0f, 100.0f, 25.0f, format::pct);
    addF ("rvLevel", "Reverb Level", -40.0f, 6.0f, -10.0f, format::level);
    addF ("rvChar", "Reverb Character", 0.0f, 100.0f, 50.0f, format::pct);
    addF ("rvDiff", "Reverb Diffusion", 0.0f, 100.0f, 80.0f, format::pct);
    addF ("rvLo", "Reverb Low Cut", 20.0f, 1000.0f, 120.0f, format::lowCut, 160.0f);
    addF ("rvHi", "Reverb High Cut", 1000.0f, 20000.0f, 12000.0f, format::highCut, 6000.0f);
    addF ("rvWidth", "Reverb Width", 0.0f, 150.0f, 100.0f, format::pct);
    addB ("rvFreeze", "Reverb Freeze", false);

    // Ducker: turns the effects down while the dry signal (or a key) is playing
    addB ("duckOn", "Ducker On", false);
    addF ("duckAmt", "Ducker Amount", 0.0f, 24.0f, 8.0f, format::db);
    addF ("duckThr", "Ducker Threshold", -60.0f, 0.0f, -30.0f, format::db);
    addF ("duckRel", "Ducker Release", 20.0f, 2000.0f, 300.0f, format::ms, 300.0f);
    addC ("duckTarget", "Ducker Target", { "Both", "Delay", "Reverb" }, 0);
    addB ("duckExt", "Ducker Ext Key", false);

    // Output
    addF ("mix", "Mix", 0.0f, 100.0f, 50.0f, format::pct);
    addB ("wetOnly", "Wet Only", false);
    addF ("outGain", "Output Trim", -24.0f, 24.0f, 0.0f, format::db);

    return layout;
}
} // namespace reverie
