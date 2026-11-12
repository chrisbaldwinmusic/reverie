#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "DSP/Common.h"
#include "DSP/Delay.h"
#include "DSP/Reverb.h"
#include "Parameters.h"

class ReverieProcessor final : public juce::AudioProcessor
{
public:
    ReverieProcessor();
    ~ReverieProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return "Reverie"; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    bool isMidiEffect() const override                      { return false; }
    double getTailLengthSeconds() const override;
    juce::AudioProcessorParameter* getBypassParameter() const override  { return bypassParam; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Version of the saved-state format. Raise it whenever a saved session or preset would load
        differently (a parameter renamed, a range or choice list changed) and add a step to
        migrateState in PluginProcessor.cpp that converts the previous version. */
    static constexpr int stateVersion = 1;
    static constexpr const char* stateVersionId = "stateVersion";

    /** The settings as saved in sessions and presets, stamped with the format version. */
    juce::ValueTree stampedState();
    /** Restores settings saved by any version of Reverie. Returns false if the tree is not a Reverie state. */
    bool loadState (juce::ValueTree tree);

    juce::UndoManager undo;
    juce::AudioProcessorValueTreeState apvts;

    /** A/B comparison: two complete settings snapshots held for the life of the plugin instance. */
    int  getABSlot() const noexcept     { return abSlot; }
    void switchAB (int slot);

    /** The tempo the synced delay follows: the host's when it gives one, otherwise the Tempo control. */
    double currentBpm() const noexcept;
    /** Left and right delay times in milliseconds, after sync and offset. */
    void delayTimesMs (float& left, float& right) const noexcept;

    /** Written on the audio thread, read (and peak-reset) by the editor. */
    struct Meters
    {
        std::atomic<float> inPeak[2] { { 0.0f }, { 0.0f } }, outPeak[2] { { 0.0f }, { 0.0f } };
        std::atomic<float> vuRms { 0.0f };
        std::atomic<float> grDuck { 0.0f };
        std::atomic<float> delayPeak { 0.0f }, reverbPeak { 0.0f };    // level each effect is adding
        std::atomic<float> feedbackPeak { 0.0f };                       // above 1, the delay's loop limiter is working
        std::atomic<float> lufsS { -100.0f }, lufsM { -100.0f };
        std::atomic<float> hostBpm { 0.0f };                            // 0 when the host gives no tempo
        std::atomic<float> keyPeak { 0.0f };                            // level arriving at the sidechain input
        std::atomic<bool>  keyConnected { false };                      // the host has given us a sidechain bus
    } meters;

private:
    // chunk-rate smoothed parameters
    enum Sm
    {
        sFeedLo, sFeedHi, sDlyOffset, sDlyFb, sDlyLo, sDlyHi, sDlyModRate, sDlyModDepth, sDlyChar, sDlyLevel,
        sRvPre, sRvSize, sRvDecay, sRvDamp, sRvMod, sRvLevel, sRvChar, sRvDiff, sRvLo, sRvHi, sRvWidth,
        sDuckAmt, sDuckThr, sDuckRel, sMix,
        numSmoothed
    };

    void processChunk (float* l, float* r, const float* keyL, const float* keyR, int n);
    void processEffects (float* l, float* r, const float* keyL, const float* keyR, int n);

    std::atomic<float>* smSrc[numSmoothed] {};
    float sm[numSmoothed] {};
    float smCoef = 0.1f;
    bool smPrimed = false;

    std::atomic<float> *pRoute {}, *pBypass {}, *pInGain {}, *pOutGain {}, *pBpm {};
    std::atomic<float> *pDlyOn {}, *pDlyType {}, *pDlyMode {}, *pDlySync {}, *pDlyTime {}, *pDlyDiv {}, *pDlyOffset {}, *pDlyFb {};
    std::atomic<float> *pRvOn {}, *pRvType {}, *pRvDecay {}, *pRvFreeze {};
    std::atomic<float> *pDuckOn {}, *pDuckTarget {}, *pDuckExt {}, *pWetOnly {};
    juce::AudioParameterBool* bypassParam = nullptr;

    double sr = 48000.0;
    int chunk = 32;
    bool prepared = false;

    juce::SmoothedValue<float> inGain, outGain;
    reverie::Biquad feedHp, feedLp;
    reverie::Echo echo;
    reverie::Reverb verb;
    reverie::Ducker ducker;
    reverie::LoudnessMeter loudness;

    // click-free switching
    reverie::Fade bypassFade, feedHpFade, feedLpFade, delayFade, reverbFade, routeFade;

    // gains that ramp across each chunk
    float delayGain = 0.0f, reverbGain = 0.0f, dryGain = 1.0f, wetGain = 1.0f, duckDelay = 1.0f, duckReverb = 1.0f;
    bool gainsPrimed = false;

    std::vector<float> monoR, dryL, dryR, feedL, feedR, tmpL, tmpR, sendL, sendR, dlyL, dlyR, rvL, rvR, zero, duckGain;
    float vuState = 0.0f, vuCoef = 0.001f;

    juce::ValueTree abState[2];
    int abSlot = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverieProcessor)
};
