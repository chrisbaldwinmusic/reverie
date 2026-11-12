// Offline checks for Reverie: runs audio through the processor and checks the results.
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <CoreFoundation/CoreFoundation.h>
#include <iostream>

namespace
{
int failures = 0;
const double sr = 48000.0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

void setParam (ReverieProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    jassert (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

void resetParams (ReverieProcessor& p)
{
    for (auto* param : p.getParameters())
        param->setValueNotifyingHost (param->getDefaultValue());
}

void prepare (ReverieProcessor& p, int channels = 2, double rate = sr, int block = 512)
{
    p.setPlayConfigDetails (channels, channels, rate, block);
    p.prepareToPlay (rate, block);
}

void run (ReverieProcessor& p, juce::AudioBuffer<float>& audio, int blockSize)
{
    juce::MidiBuffer midi;
    for (int pos = 0; pos < audio.getNumSamples(); pos += blockSize)
    {
        const int n = juce::jmin (blockSize, audio.getNumSamples() - pos);
        juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), audio.getNumChannels(), pos, n);
        p.processBlock (view, midi);
    }
}

juce::AudioBuffer<float> silence (int channels, int n)
{
    juce::AudioBuffer<float> b (channels, n);
    b.clear();
    return b;
}

juce::AudioBuffer<float> sine (int channels, int n, double freq, float gainDb)
{
    juce::AudioBuffer<float> b (channels, n);
    const float g = juce::Decibels::decibelsToGain (gainDb);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < n; ++i)
            b.setSample (ch, i, g * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr));
    return b;
}

/** Noise for the first `burst` samples (the whole buffer when burst < 0), silence after. */
juce::AudioBuffer<float> noise (int channels, int n, float gainDb, int seed = 1, int burst = -1)
{
    juce::AudioBuffer<float> b (channels, n);
    b.clear();
    juce::Random rng (seed);
    const float g = juce::Decibels::decibelsToGain (gainDb);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < (burst < 0 ? n : juce::jmin (n, burst)); ++i)
            b.setSample (ch, i, g * (rng.nextFloat() * 2.0f - 1.0f));
    return b;
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (ch, i))) return false;
    return true;
}

float peak (const juce::AudioBuffer<float>& b, int ch, int from, int to)
{
    return b.getMagnitude (ch, from, juce::jmin (to, b.getNumSamples()) - from);
}

float rmsDb (const juce::AudioBuffer<float>& b, int from, int to, int ch = 0)
{
    return juce::Decibels::gainToDecibels (b.getRMSLevel (ch, from, juce::jmin (to, b.getNumSamples()) - from), -200.0f);
}

int peakIndex (const juce::AudioBuffer<float>& b, int ch, int from, int to)
{
    int at = from; float pk = 0.0f;
    for (int i = from; i < juce::jmin (to, b.getNumSamples()); ++i)
        if (std::abs (b.getSample (ch, i)) > pk) { pk = std::abs (b.getSample (ch, i)); at = i; }
    return at;
}

/** A clean echo to measure: delay only, effects only, nothing colouring the repeats. */
void cleanDelay (ReverieProcessor& p)
{
    setParam (p, "rvOn", 0.0f); setParam (p, "wetOnly", 1.0f); setParam (p, "dlyLevel", 0.0f);
    setParam (p, "dlyLo", 20.0f); setParam (p, "dlyHi", 20000.0f); setParam (p, "dlyFb", 0.0f);
}

/** Reverb only, effects only, with nothing shaping the tail but Decay. */
void cleanReverb (ReverieProcessor& p)
{
    setParam (p, "dlyOn", 0.0f); setParam (p, "wetOnly", 1.0f); setParam (p, "rvLevel", 0.0f);
    setParam (p, "rvLo", 20.0f); setParam (p, "rvHi", 20000.0f); setParam (p, "rvDamp", 20000.0f);
    setParam (p, "rvMod", 0.0f); setParam (p, "rvChar", 0.0f); setParam (p, "rvPre", 0.0f);
}

/** Reverb time from the decay of the tail (Schroeder integration, -5 to -25 dB, extrapolated to 60). */
float measureRt60 (const juce::AudioBuffer<float>& b, int from)
{
    const int n = b.getNumSamples();
    std::vector<double> remaining ((size_t) n + 1, 0.0);
    for (int i = n; --i >= from;)
        remaining[(size_t) i] = remaining[(size_t) i + 1] + (double) b.getSample (0, i) * b.getSample (0, i);
    const double total = remaining[(size_t) from];
    if (total <= 0.0) return 0.0f;

    int t5 = -1, t25 = -1;
    for (int i = from; i < n; ++i)
    {
        const double db = 10.0 * std::log10 (std::max (1.0e-30, remaining[(size_t) i] / total));
        if (t5 < 0 && db <= -5.0) t5 = i;
        if (db <= -25.0) { t25 = i; break; }
    }
    return t5 < 0 || t25 < 0 ? 0.0f : (float) (3.0 * (t25 - t5) / sr);
}

struct FixedTempo final : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        return info;
    }
};
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "shots");
    outDir.createDirectory();

    // REVERIE_ONLY=<text> runs just the sections whose heading contains the text
    auto want = [] (const char* section)
    {
        const char* only = std::getenv ("REVERIE_ONLY");
        const bool yes = only == nullptr || juce::String (section).containsIgnoreCase (only);
        if (yes) std::cout << section << std::endl;
        return yes;
    };

    static const char* delayTypes[] = { "Digital", "Tape", "Analog", "Lo-Fi", "Reverse" };
    static const char* reverbTypes[] = { "Room", "Chamber", "Hall", "Plate", "Spring", "Shimmer" };

    if (want ("Bypass and dry path"))
    {
        ReverieProcessor p;
        prepare (p);
        setParam (p, "dlyFb", 80.0f);
        setParam (p, "bypass", 1.0f);
        const auto in = noise (2, 96000, -12.0f);
        auto out = in;
        run (p, out, 512);
        float err = 0.0f;
        for (int i = 0; i < out.getNumSamples(); ++i)
            err = std::max (err, std::abs (out.getSample (0, i) - in.getSample (0, i)));
        check (p.getLatencySamples() == 0 && err < 1.0e-6f, "bypass is the input exactly, with no latency");

        ReverieProcessor q;
        prepare (q);
        setParam (q, "dlyOn", 0.0f); setParam (q, "rvOn", 0.0f);
        out = in;
        run (q, out, 512);
        err = 0.0f;
        for (int i = 0; i < out.getNumSamples(); ++i)
            err = std::max (err, std::abs (out.getSample (1, i) - in.getSample (1, i)));
        check (err < 1.0e-6f, "with both effects off at Mix 50 % the dry signal passes at unity");

        setParam (q, "wetOnly", 1.0f);
        out = in;
        run (q, out, 512);
        check (peak (out, 0, 4800, 96000) < 1.0e-5f, "Wet Only removes the dry signal");
    }

    if (want ("Delay timing"))
    {
        ReverieProcessor p;
        prepare (p);
        cleanDelay (p);
        setParam (p, "dlyTime", 250.0f); setParam (p, "dlyFb", 50.0f);
        auto b = silence (2, 96000);
        b.setSample (0, 1000, 0.5f); b.setSample (1, 1000, 0.5f);
        run (p, b, 512);
        const int first = peakIndex (b, 0, 1100, 20000), second = peakIndex (b, 0, 20000, 30000);
        std::cout << "  250 ms: first echo after " << (first - 1000) << " samples, second after " << (second - 1000) << std::endl;
        check (std::abs (first - 1000 - 12000) <= 1 && std::abs (second - 1000 - 24000) <= 1, "echoes land on the set time");
        const float ratio = rmsDb (b, 24900, 25200) - rmsDb (b, 12900, 13200);
        std::cout << "  second echo is " << ratio << " dB below the first at 50 % feedback" << std::endl;
        check (std::abs (ratio + 6.02f) < 0.3f, "each repeat is quieter by the feedback amount");

        // synced to the Tempo control when there is no host tempo
        ReverieProcessor q;
        prepare (q);
        cleanDelay (q);
        setParam (q, "dlySync", 1.0f); setParam (q, "dlyDiv", 6.0f); setParam (q, "bpm", 120.0f);     // dotted eighth
        b = silence (2, 96000);
        b.setSample (0, 1000, 0.5f); b.setSample (1, 1000, 0.5f);
        run (q, b, 512);
        check (std::abs (peakIndex (b, 0, 1100, 96000) - 1000 - 18000) <= 1, "dotted eighth at 120 BPM is 375 ms");

        // and to the host when it supplies one
        ReverieProcessor h;
        FixedTempo head;
        head.bpm = 90.0;
        h.setPlayHead (&head);
        prepare (h);
        cleanDelay (h);
        setParam (h, "dlySync", 1.0f); setParam (h, "dlyDiv", 8.0f);
        b = silence (2, 96000);
        run (h, b, 512);        // one pass so the tempo has been read before the echo is timed
        b.clear();
        b.setSample (0, 1000, 0.5f); b.setSample (1, 1000, 0.5f);
        run (h, b, 512);
        check (std::abs (peakIndex (b, 0, 1100, 96000) - 1000 - 32000) <= 1, "quarter note follows a host tempo of 90 BPM (667 ms)");
        h.setPlayHead (nullptr);
    }

    if (want ("Delay modes"))
    {
        auto impulse = [] (int mode, float offset)
        {
            ReverieProcessor p;
            prepare (p);
            cleanDelay (p);
            setParam (p, "dlyTime", 100.0f); setParam (p, "dlyFb", 60.0f); setParam (p, "dlyMode", (float) mode); setParam (p, "dlyOffset", offset);
            auto b = silence (2, 48000);
            b.setSample (0, 1000, 0.5f);        // left only
            run (p, b, 512);
            return b;
        };
        auto at = [] (const juce::AudioBuffer<float>& b, int ch, int echo) { return peak (b, ch, 1000 + 4800 * echo - 50, 1000 + 4800 * echo + 50); };

        auto b = impulse (0, 0.0f);
        check (at (b, 0, 1) > 0.2f && at (b, 0, 2) > 0.1f && peak (b, 1, 0, 48000) < 1.0e-5f, "Stereo: a left-only sound echoes on the left only");
        b = impulse (1, 0.0f);
        check (at (b, 0, 1) > 0.1f && at (b, 1, 1) < 1.0e-4f && at (b, 1, 2) > 0.05f && at (b, 0, 2) < 1.0e-4f && at (b, 0, 3) > 0.03f,
               "Ping-Pong: left, right, left");
        b = impulse (2, 0.0f);
        check (at (b, 0, 1) > 0.2f && at (b, 1, 1) < 1.0e-4f && at (b, 1, 2) > 0.1f && at (b, 0, 2) < 1.0e-4f, "Cross: each repeat swaps sides");

        ReverieProcessor p;
        prepare (p);
        cleanDelay (p);
        setParam (p, "dlyTime", 100.0f); setParam (p, "dlyOffset", 50.0f);
        b = silence (2, 48000);
        b.setSample (0, 1000, 0.5f); b.setSample (1, 1000, 0.5f);
        run (p, b, 512);
        check (std::abs (peakIndex (b, 0, 1100, 48000) - 5800) <= 1 && std::abs (peakIndex (b, 1, 1100, 48000) - 8200) <= 1,
               "L/R Offset of +50 % makes the right side half as long again");
    }

    if (want ("Delay types and runaway feedback"))
    {
        for (int type = 0; type < 5; ++type)
        {
            ReverieProcessor p;
            prepare (p);
            cleanDelay (p);
            setParam (p, "dlyType", (float) type); setParam (p, "dlyTime", 180.0f); setParam (p, "dlyFb", 110.0f);
            setParam (p, "dlyChar", 100.0f); setParam (p, "dlyModDepth", 60.0f);
            auto b = noise (2, (int) sr * 20, -6.0f, 3 + type, (int) sr);
            run (p, b, 512);
            const float early = rmsDb (b, (int) sr * 2, (int) sr * 3), late = rmsDb (b, (int) sr * 18, (int) sr * 20);
            std::cout << "  " << delayTypes[type] << " at 110 % feedback: peak " << peak (b, 0, 0, b.getNumSamples())
                      << ", level after 2 s " << early << " dB, after 19 s " << late << " dB" << std::endl;
            check (allFinite (b) && peak (b, 0, 0, b.getNumSamples()) < 4.0f && early > -60.0f,
                   juce::String (delayTypes[type]) + " makes echoes and stays bounded at 110 % feedback");
        }

        // the repeats of the coloured types get duller; the digital ones do not
        auto brightness = [] (int type)
        {
            ReverieProcessor p;
            prepare (p);
            cleanDelay (p);
            setParam (p, "dlyType", (float) type); setParam (p, "dlyTime", 200.0f); setParam (p, "dlyFb", 80.0f); setParam (p, "dlyChar", 80.0f);
            auto b = noise (2, 96000, -12.0f, 9, 4800);
            run (p, b, 512);
            auto hf = [&b] (int from)       // share of the energy above about 5 kHz, via a first difference
            {
                double diff = 0.0, all = 0.0;
                for (int i = from + 1; i < from + 4800; ++i)
                {
                    const double d = b.getSample (0, i) - b.getSample (0, i - 1);
                    diff += d * d; all += (double) b.getSample (0, i) * b.getSample (0, i);
                }
                return (float) (10.0 * std::log10 (std::max (1.0e-20, diff) / std::max (1.0e-20, all)));
            };
            return hf (9600 * 4) - hf (9600);      // fourth repeat against the first
        };
        const float dig = brightness (0), tape = brightness (1), analog = brightness (2);
        std::cout << "  top end of repeat 4 against repeat 1: Digital " << dig << " dB, Tape " << tape << " dB, Analog " << analog << " dB" << std::endl;
        // The bucket brigade's steep anti-aliasing filters already band-limit the first repeat, so later passes
        // darken it by less than tape does; the per-stage charge loss still makes each one clearly duller.
        check (std::abs (dig) < 1.0f && tape < -3.0f && analog < dig - 1.5f, "Tape and Analog repeats darken as they go round; Digital ones do not");
    }

    if (want ("Reverb decay time"))
    {
        for (int type = 0; type < 6; ++type)
            for (float decay : { 0.8f, 2.0f, 5.0f })
            {
                ReverieProcessor p;
                prepare (p);
                cleanReverb (p);
                setParam (p, "rvType", (float) type); setParam (p, "rvDecay", decay);
                auto b = noise (2, (int) (sr * (decay * 1.6 + 1.0)), -6.0f, 21 + type, 2400);
                run (p, b, 512);
                const float rt = measureRt60 (b, 4800);
                std::cout << "  " << reverbTypes[type] << ", Decay " << decay << " s: measured " << rt << " s" << std::endl;
                check (allFinite (b) && std::abs (rt / decay - 1.0f) < 0.2f, juce::String (reverbTypes[type]) + " decays in the time set");
            }

        // damping shortens the top of the tail, not the bottom
        ReverieProcessor p;
        prepare (p);
        cleanReverb (p);
        setParam (p, "rvDecay", 3.0f); setParam (p, "rvDamp", 2000.0f);
        auto b = noise (2, (int) sr * 5, -6.0f, 40, 2400);
        run (p, b, 512);
        auto diffDb = [&b] (int from, int to)
        {
            double diff = 0.0, all = 0.0;
            for (int i = from + 1; i < to; ++i)
            {
                const double d = b.getSample (0, i) - b.getSample (0, i - 1);
                diff += d * d; all += (double) b.getSample (0, i) * b.getSample (0, i);
            }
            return (float) (10.0 * std::log10 (std::max (1.0e-30, diff) / std::max (1.0e-30, all)));
        };
        const float darkening = diffDb ((int) (sr * 1.5), (int) (sr * 2.0)) - diffDb (4800, 14400);
        std::cout << "  Damping at 2 kHz: the tail is " << darkening << " dB duller after 1.5 s than at the start" << std::endl;
        check (darkening < -6.0f, "Damping makes the tail darken as it decays");
    }

    if (want ("Reverb freeze, pre-delay and width"))
    {
        ReverieProcessor p;
        prepare (p);
        cleanReverb (p);
        setParam (p, "rvMod", 60.0f); setParam (p, "rvDamp", 4000.0f);
        auto b = noise (2, (int) sr, -12.0f, 5);
        run (p, b, 512);
        setParam (p, "rvFreeze", 1.0f);
        auto held = noise (2, (int) sr * 30, -12.0f, 6);       // audio keeps arriving, but must not enter the frozen tail
        run (p, held, 512);
        const float early = rmsDb (held, (int) sr * 2, (int) sr * 3), late = rmsDb (held, (int) sr * 29, (int) sr * 30);
        std::cout << "  frozen tail: " << early << " dB after 2 s, " << late << " dB after 29 s" << std::endl;
        check (allFinite (held) && early > -60.0f && std::abs (late - early) < 0.5f, "Freeze holds the tail without it fading or growing");
        setParam (p, "rvFreeze", 0.0f);
        auto after = silence (2, (int) sr * 6);
        run (p, after, 512);
        check (rmsDb (after, (int) sr * 5, (int) sr * 6) < early - 50.0f, "releasing Freeze lets the tail die away");

        ReverieProcessor q;
        prepare (q);
        cleanReverb (q);
        setParam (q, "rvPre", 100.0f);
        b = silence (2, 48000);
        b.setSample (0, 1000, 0.5f); b.setSample (1, 1000, 0.5f);
        run (q, b, 512);
        check (peak (b, 0, 0, 1000 + 4790) < 1.0e-6f && peak (b, 0, 1000 + 4800, 1000 + 9600) > 1.0e-3f, "Pre-Delay holds the reverb back by the time set");

        ReverieProcessor w;
        prepare (w);
        cleanReverb (w);
        setParam (w, "rvWidth", 0.0f);
        b = noise (2, 48000, -12.0f, 8);
        run (w, b, 512);
        float diff = 0.0f;
        for (int i = 0; i < b.getNumSamples(); ++i) diff = std::max (diff, std::abs (b.getSample (0, i) - b.getSample (1, i)));
        check (diff < 1.0e-6f, "Width at 0 % is mono");
    }

    if (want ("Shimmer stays bounded"))
    {
        ReverieProcessor p;
        prepare (p);
        cleanReverb (p);
        setParam (p, "rvType", 5.0f); setParam (p, "rvDecay", 30.0f); setParam (p, "rvChar", 100.0f); setParam (p, "rvSize", 100.0f);
        auto b = noise (2, (int) sr * 40, -6.0f, 12);
        run (p, b, 512);
        std::cout << "  40 s of loud noise, Decay 30 s, Shimmer 100 %: peak " << peak (b, 0, 0, b.getNumSamples())
                  << ", level over the last 5 s " << rmsDb (b, (int) sr * 35, (int) sr * 40) << " dB" << std::endl;
        check (allFinite (b) && peak (b, 0, 0, b.getNumSamples()) < 8.0f, "the octave-up feedback never runs away");
    }

    if (want ("Ducker"))
    {
        auto wetLevel = [] (bool duck, int target)
        {
            ReverieProcessor p;
            prepare (p);
            setParam (p, "wetOnly", 1.0f); setParam (p, "dlyOn", 0.0f);
            setParam (p, "duckOn", duck ? 1.0f : 0.0f); setParam (p, "duckAmt", 12.0f); setParam (p, "duckThr", -30.0f);
            setParam (p, "duckTarget", (float) target);
            auto b = noise (2, (int) sr * 4, -12.0f, 14);
            run (p, b, 512);
            return rmsDb (b, (int) sr * 2, (int) sr * 4);
        };
        const float open = wetLevel (false, 0), ducked = wetLevel (true, 0), delayOnly = wetLevel (true, 1);
        std::cout << "  reverb level " << open << " dB, ducked " << ducked << " dB, with the ducker aimed at the delay " << delayOnly << " dB" << std::endl;
        check (std::abs ((open - ducked) - 12.0f) < 1.0f, "a signal well above the threshold ducks the effects by the Amount");
        check (std::abs (open - delayOnly) < 0.2f, "Target: Delay leaves the reverb alone");

        // external key
        auto keyed = [] (float keyDb)
        {
            ReverieProcessor p;
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (juce::AudioChannelSet::stereo());
            layout.inputBuses.add (juce::AudioChannelSet::stereo());
            layout.outputBuses.add (juce::AudioChannelSet::stereo());
            const bool ok = p.setBusesLayout (layout);
            p.setRateAndBufferSizeDetails (sr, 512);
            p.prepareToPlay (sr, 512);
            setParam (p, "wetOnly", 1.0f); setParam (p, "dlyOn", 0.0f);
            setParam (p, "duckOn", 1.0f); setParam (p, "duckAmt", 12.0f); setParam (p, "duckThr", -30.0f); setParam (p, "duckExt", 1.0f);

            juce::AudioBuffer<float> b (4, (int) sr * 4);
            const auto main = noise (2, b.getNumSamples(), -12.0f, 15), key = sine (2, b.getNumSamples(), 200.0, keyDb);
            for (int ch = 0; ch < 2; ++ch)
            {
                b.copyFrom (ch, 0, main, ch, 0, b.getNumSamples());
                b.copyFrom (ch + 2, 0, key, ch, 0, b.getNumSamples());
            }
            run (p, b, 512);
            return ok ? rmsDb (b, (int) sr * 2, (int) sr * 4) : 999.0f;
        };
        const float quietKey = keyed (-80.0f), loudKey = keyed (-6.0f);
        std::cout << "  external key: quiet key " << quietKey << " dB, loud key " << loudKey << " dB" << std::endl;
        check (std::abs (quietKey - open) < 0.5f && std::abs ((quietKey - loudKey) - 12.0f) < 1.0f, "with Ext Key on, the sidechain input does the ducking");
    }

    if (want ("Routing"))
    {
        auto tailAfterEcho = [] (int route)
        {
            ReverieProcessor p;
            prepare (p);
            setParam (p, "route", (float) route); setParam (p, "wetOnly", 1.0f);
            setParam (p, "dlyTime", 500.0f); setParam (p, "dlyFb", 0.0f); setParam (p, "dlyLevel", 0.0f); setParam (p, "rvDecay", 0.4f); setParam (p, "rvPre", 0.0f);
            auto b = noise (2, 96000, -6.0f, 17, 480);
            run (p, b, 512);
            // 500 ms in: the echo has just played. Is there fresh reverb after it?
            return rmsDb (b, 24000 + 2400, 24000 + 12000);
        };
        const float series = tailAfterEcho (0), parallel = tailAfterEcho (1), reversed = tailAfterEcho (2);
        std::cout << "  level just after the echo: Dly>Rev " << series << " dB, Parallel " << parallel << " dB, Rev>Dly " << reversed << " dB" << std::endl;
        check (series > parallel + 10.0f, "Delay into Reverb puts reverb on the echo; Parallel does not");
        check (reversed > parallel + 10.0f, "Reverb into Delay repeats the reverb tail");
    }

    if (want ("Click-free switching (100 Hz tone, largest sample-to-sample step)"))
    {
        struct Flip { const char* id; float a, b; };
        static const Flip flips[] = {
            { "bypass", 0, 1 }, { "wetOnly", 0, 1 }, { "dlyOn", 1, 0 }, { "rvOn", 1, 0 }, { "duckOn", 0, 1 }, { "rvFreeze", 0, 1 },
            { "route", 0, 1 }, { "route", 0, 2 }, { "dlyType", 0, 1 }, { "dlyType", 0, 3 }, { "dlyType", 1, 4 }, { "dlyMode", 0, 1 }, { "dlyMode", 0, 2 },
            { "rvType", 2, 0 }, { "rvType", 2, 4 }, { "rvType", 3, 5 }, { "duckTarget", 0, 2 }, { "duckExt", 0, 1 },
            { "feedLo", 20, 300 }, { "feedHi", 20000, 3000 }, { "dlyLevel", -40, 0 }, { "rvLevel", -40, 0 }, { "mix", 0, 100 },
            { "dlySync", 0, 1 }, { "rvSize", 20, 90 }, { "rvPre", 0, 120 } };

        float worst = 0.0f, steadyWorst = 0.0f; juce::String worstName;
        for (const auto& f : flips)
        {
            auto setUp = [&f] (ReverieProcessor& q, float value)
            {
                prepare (q);
                setParam (q, "dlyFb", 60.0f); setParam (q, "dlyLevel", -3.0f); setParam (q, "rvLevel", -3.0f);
                setParam (q, "dlyTime", 130.0f); setParam (q, "duckAmt", 12.0f);
                setParam (q, f.id, value);
            };

            // how steppy the output is with the control left at its second setting (Lo-Fi, say, is steppy by design)
            float steady = 0.0f;
            {
                ReverieProcessor q;
                setUp (q, f.b);
                auto t = sine (2, 36000, 100.0, -12.0f);
                for (int i = 0; i < t.getNumSamples(); ++i) t.setSample (1, i, t.getSample (1, i) * 0.8f);
                run (q, t, 480);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 6000; i < t.getNumSamples(); ++i)
                        steady = std::max (steady, std::abs (t.getSample (ch, i) - t.getSample (ch, i - 1)));
            }

            ReverieProcessor p;
            setUp (p, f.a);

            // settle, then flip the control back and forth every quarter of a second
            auto b = sine (2, 12000 * 6, 100.0, -12.0f);
            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (1, i, b.getSample (1, i) * 0.8f);
            for (int seg = 0; seg < 6; ++seg)
            {
                if (seg >= 2) setParam (p, f.id, seg % 2 == 0 ? f.b : f.a);
                juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, seg * 12000, 12000);
                run (p, view, 480);
            }

            float step = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 6000; i < b.getNumSamples(); ++i)
                {
                    const float d = std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1));
                    if (i < 24000) steady = std::max (steady, d); else step = std::max (step, d);
                }
            steadyWorst = std::max (steadyWorst, steady);
            if (step > worst) { worst = step; worstName = juce::String (f.id) + " " + juce::String (f.a) + " <> " + juce::String (f.b); }
            if (! (allFinite (b) && step < std::max (0.03f, 2.5f * steady)))
                check (false, juce::String (f.id) + " " + juce::String (f.a) + " <> " + juce::String (f.b) + ": step " + juce::String (step, 4)
                                  + " against " + juce::String (steady, 4) + " when left alone");
        }
        std::cout << "  largest step while switching " << worst << " at " << worstName << "; largest in steady playback " << steadyWorst << std::endl;
        check (true, "switching matrix completed");
    }

    if (want ("Stress: every type / routing / rate / block size / layout"))
    {
        int runs = 0;
        bool ok = true;
        for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (int block : { 1, 37, 256, 2048 })
                for (int channels : { 1, 2 })
                    for (int combo = 0; combo < 6; ++combo)
                    {
                        if (block == 1 && rate > 48000.0) continue;
                        ReverieProcessor p;
                        prepare (p, channels, rate, block);
                        setParam (p, "route", (float) (combo % 3));
                        setParam (p, "dlyType", (float) (combo % 5)); setParam (p, "dlyMode", (float) (combo % 3));
                        setParam (p, "rvType", (float) combo);
                        setParam (p, "dlyFb", 105.0f); setParam (p, "dlyModDepth", 100.0f); setParam (p, "dlyChar", 100.0f);
                        setParam (p, "rvDecay", 20.0f); setParam (p, "rvChar", 100.0f); setParam (p, "rvMod", 100.0f);
                        setParam (p, "duckOn", 1.0f); setParam (p, "feedLo", 200.0f); setParam (p, "feedHi", 5000.0f);
                        setParam (p, "dlySync", (float) (combo & 1)); setParam (p, "bpm", 40.0f); setParam (p, "dlyDiv", 13.0f);

                        auto b = noise (channels, (int) (rate * (block == 1 ? 0.5 : 2.0)), 0.0f, 50 + combo);
                        const int half = b.getNumSamples() / 2;
                        juce::AudioBuffer<float> first (b.getArrayOfWritePointers(), channels, 0, half);
                        run (p, first, block);
                        setParam (p, "dlyTime", 5.0f); setParam (p, "rvSize", 100.0f); setParam (p, "rvFreeze", 1.0f); setParam (p, "dlySync", 0.0f);
                        juce::AudioBuffer<float> second (b.getArrayOfWritePointers(), channels, half, b.getNumSamples() - half);
                        run (p, second, block);
                        ++runs;
                        if (! allFinite (b) || b.getMagnitude (0, b.getNumSamples()) > 20.0f)
                        {
                            ok = false;
                            check (false, juce::String (rate) + " Hz, block " + juce::String (block) + ", " + juce::String (channels) + " ch, combination " + juce::String (combo));
                        }
                    }
        std::cout << "  " << runs << " runs" << std::endl;
        check (ok, "stress matrix completed with finite, bounded output");
    }

    if (want ("CPU"))
    {
        for (int type = 0; type < 6; ++type)
        {
            ReverieProcessor p;
            prepare (p, 2, sr, 256);
            setParam (p, "rvType", (float) type); setParam (p, "dlyType", (float) (type % 5));
            setParam (p, "dlyModDepth", 50.0f); setParam (p, "duckOn", 1.0f); setParam (p, "rvChar", 80.0f);
            auto b = noise (2, (int) sr * 20, -12.0f, 5);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            run (p, b, 256);
            const double ms = juce::Time::getMillisecondCounterHiRes() - start;
            std::cout << "  " << delayTypes[type % 5] << " delay + " << reverbTypes[type] << " reverb: " << ms / 200.0 << " % of one core at 48 kHz" << std::endl;
            check (ms / 200.0 < 10.0, "CPU use is reasonable");
        }
    }

    if (want ("Guide starting points"))
    {
        bool ok = true;
        std::vector<reverie::guide::Source> all (reverie::guide::sources);
        all.insert (all.end(), reverie::guide::effects.begin(), reverie::guide::effects.end());
        for (auto& src : all)
        {
            ReverieProcessor p;
            prepare (p);
            for (auto& [id, value] : src.settings)
                if (p.apvts.getParameter (id) == nullptr) { ok = false; check (false, juce::String (src.name) + " names an unknown control: " + id); }
            reverie::ui::applyStartingPoint (p, src);
            auto b = noise (2, (int) sr * 6, -14.0f, 61, (int) sr * 2);
            run (p, b, 512);
            // the short window catches chorus-length delays, which are over within a few milliseconds
            const float out = rmsDb (b, 0, (int) sr * 2);
            const float tail = std::max (rmsDb (b, (int) sr * 2 + 24, (int) sr * 2 + 960), rmsDb (b, (int) (sr * 2.05), (int) (sr * 2.5)));
            std::cout << "  " << juce::String (src.name).paddedRight (' ', 18) << " level " << juce::String (out, 1) << " dB, effects just after the sound stops "
                      << juce::String (tail, 1) << " dB" << std::endl;
            if (! allFinite (b) || tail < -70.0f || tail > out)
            {
                ok = false;
                check (false, juce::String (src.name) + ": effects should be audible after the sound stops, and quieter than it");
            }
        }
        for (auto* param : ReverieProcessor().getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (param))
                if (reverie::guide::findHelp (r->paramID.toRawUTF8()) == nullptr) { ok = false; check (false, "no Guide entry for " + r->paramID); }
        check (ok, "every starting point and factory preset applies, makes an audible effect quieter than the source, and every control has a Guide entry");
    }

    if (want ("State round-trip"))
    {
        ReverieProcessor p;
        prepare (p);
        setParam (p, "route", 2.0f); setParam (p, "rvDecay", 9.0f); setParam (p, "dlyType", 3.0f);
        juce::MemoryBlock state;
        p.getStateInformation (state);
        resetParams (p);
        p.setStateInformation (state.getData(), (int) state.getSize());
        check (std::abs (p.apvts.getRawParameterValue ("rvDecay")->load() - 9.0f) < 0.01f
                   && (int) p.apvts.getRawParameterValue ("route")->load() == 2
                   && (int) p.apvts.getRawParameterValue ("dlyType")->load() == 3, "state restores");

        setParam (p, "rvDecay", 1.0f);
        p.switchAB (1);
        setParam (p, "rvDecay", 4.0f);
        p.switchAB (0);
        const float a = p.apvts.getRawParameterValue ("rvDecay")->load();
        p.switchAB (1);
        check (std::abs (a - 1.0f) < 0.01f && std::abs (p.apvts.getRawParameterValue ("rvDecay")->load() - 4.0f) < 0.01f, "A/B keeps two separate settings");
    }

    if (want ("State versions"))
    {
        auto value = [] (ReverieProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
        auto load = [] (ReverieProcessor& p, const juce::ValueTree& tree)
        {
            juce::MemoryBlock block;
            p.copyXmlToBinary (*tree.createXml(), block);
            p.setStateInformation (block.getData(), (int) block.getSize());
        };

        ReverieProcessor p;
        prepare (p);
        setParam (p, "rvDecay", 7.0f); setParam (p, "dlyFb", 60.0f);
        juce::MemoryBlock block;
        p.getStateInformation (block);
        const auto saved = juce::ValueTree::fromXml (*p.getXmlFromBinary (block.getData(), (int) block.getSize()));
        check ((int) saved.getProperty (ReverieProcessor::stateVersionId, 0) == ReverieProcessor::stateVersion,
               "saved state records its format version");

        // a state written before a control existed: the control goes to its default, not to whatever was set before
        auto old = saved.createCopy();
        old.removeProperty (ReverieProcessor::stateVersionId, nullptr);
        old.removeChild (old.getChildWithProperty ("id", "rvWidth"), nullptr);
        setParam (p, "rvWidth", 30.0f);
        load (p, old);
        check (std::abs (value (p, "rvWidth") - 100.0f) < 0.01f && std::abs (value (p, "rvDecay") - 7.0f) < 0.01f,
               "an older state without a control loads it at its default, and the rest as saved");

        // a state from a later version, with a control this version does not have
        auto newer = saved.createCopy();
        newer.setProperty (ReverieProcessor::stateVersionId, ReverieProcessor::stateVersion + 5, nullptr);
        juce::ValueTree extra ("PARAM");
        extra.setProperty ("id", "fromTheFuture", nullptr).setProperty ("value", 1.0f, nullptr);
        newer.appendChild (extra, nullptr);
        resetParams (p);
        load (p, newer);
        check (std::abs (value (p, "rvDecay") - 7.0f) < 0.01f && std::abs (value (p, "dlyFb") - 60.0f) < 0.01f,
               "a state from a newer version loads the controls this version knows");

        // out-of-range values are held to the control's range
        auto wild = saved.createCopy();
        wild.getChildWithProperty ("id", "dlyFb").setProperty ("value", 5000.0f, nullptr);
        load (p, wild);
        check (value (p, "dlyFb") <= 110.0f, "an out-of-range value is held to the control's range");

        // something that is not a Reverie state changes nothing
        setParam (p, "rvDecay", 3.0f);
        const char junk[] = "not a plug-in state";
        p.setStateInformation (junk, (int) sizeof (junk));
        juce::ValueTree other ("SOMETHING_ELSE");
        load (p, other);
        p.setStateInformation (nullptr, 0);
        check (std::abs (value (p, "rvDecay") - 3.0f) < 0.01f, "data that is not a Reverie state is ignored");
    }

    if (want ("Analog bucket brigade"))
    {
        auto analog = [] (ReverieProcessor& p, float ms, float character)
        {
            cleanDelay (p);
            setParam (p, "dlyType", 2.0f); setParam (p, "dlyTime", ms); setParam (p, "dlyChar", character);
        };

        // the clock runs within the chips' range at 300 ms, is held at its fastest at 20 ms and at its slowest at 1.5 s
        for (float ms : { 20.0f, 300.0f, 1500.0f })
        {
            ReverieProcessor p;
            prepare (p);
            analog (p, ms, 0.0f);
            auto in = noise (2, (int) sr * 4, -20.0f, 21);
            auto out = in;
            run (p, out, 512);
            const int expected = juce::roundToInt (ms * 0.001 * sr);
            int best = 0; double bestC = -1.0;
            for (int lag = expected - 40; lag <= expected + 40; ++lag)
            {
                double c = 0.0;
                for (int i = (int) sr * 2; i < (int) sr * 3; ++i) c += (double) in.getSample (0, i - lag) * out.getSample (0, i);
                if (c > bestC) { bestC = c; best = lag; }
            }
            std::cout << "  " << ms << " ms: echo arrives " << best - expected << " samples from the time set" << std::endl;
            check (std::abs (best - expected) <= 1, "Analog echo at " + juce::String (ms, 0) + " ms lands on the time set, filters and all");
        }

        // the compander restores the level it took away
        for (float level : { -40.0f, -25.0f, -10.0f })
        {
            ReverieProcessor p;
            prepare (p);
            analog (p, 250.0f, 0.0f);
            auto b = sine (2, (int) sr * 3, 500.0, level);
            run (p, b, 512);
            const float got = rmsDb (b, (int) sr * 2, (int) sr * 3) + 3.01f;
            std::cout << "  500 Hz at " << level << " dBFS: echo at " << got << " dBFS" << std::endl;
            check (std::abs (got - level) < 1.0f, "the compander gives back the level it took, at " + juce::String (level, 0) + " dBFS");
        }

        // bandwidth: the anti-aliasing and reconstruction filters set the top end, and the expander doubles their slope
        {
            auto echoDb = [&] (double freq)
            {
                ReverieProcessor p;
                prepare (p);
                analog (p, 250.0f, 0.0f);
                auto b = sine (2, (int) sr * 3, freq, -20.0f);
                run (p, b, 512);
                return rmsDb (b, (int) sr * 2, (int) sr * 3) + 3.01f + 20.0f;
            };
            const float mid = echoDb (2000.0), top = echoDb (12000.0);
            std::cout << "  echo level against the input: 2 kHz " << mid << " dB, 12 kHz " << top << " dB" << std::endl;
            check (std::abs (mid) < 1.0f && top < -30.0f, "Analog passes 2 kHz and removes 12 kHz");
        }

        // a change of sample rate redesigns the filters, even when Character has not moved
        {
            auto render = [&] (ReverieProcessor& p, double rate)
            {
                prepare (p, 2, rate, 512);
                juce::AudioBuffer<float> b (2, (int) rate * 3);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        b.setSample (ch, i, juce::Decibels::decibelsToGain (-20.0f) * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 2000.0 * i / rate));
                run (p, b, 512);
                return rmsDb (b, (int) rate * 2, (int) rate * 3);
            };
            ReverieProcessor moved, fresh;
            analog (moved, 250.0f, 40.0f);      // Character at its default, where the redesign used to be skipped
            analog (fresh, 250.0f, 40.0f);
            render (moved, 48000.0);
            const float after = render (moved, 96000.0), direct = render (fresh, 96000.0);
            std::cout << "  2 kHz echo at 96 kHz: after a change from 48 kHz " << after << " dB, prepared at 96 kHz " << direct << " dB" << std::endl;
            check (std::abs (after - direct) < 0.1f, "after a sample-rate change Analog sounds as if it had started at the new rate");
        }

        // hiss: the expander pulls the chips' noise down when nothing is playing
        {
            ReverieProcessor p;
            prepare (p);
            analog (p, 300.0f, 100.0f);
            auto b = noise (2, (int) sr * 4, -12.0f, 4, (int) sr);
            run (p, b, 512);
            const float floor = rmsDb (b, (int) sr * 3, (int) sr * 4);
            std::cout << "  hiss after the echoes have gone, at full Character: " << floor << " dBFS" << std::endl;
            check (floor < -95.0f, "between phrases the hiss is pulled down out of hearing");
        }
    }

    if (want ("Interface snapshots"))
    {
        // with audio running so the meters and the coach have something to show
        struct Shot { const char* name; const char* source; int view; };
        static const Shot shots[] = { { "guide", "Lead Vocal", 1 }, { "timeline", "Lead Guitar", 2 }, { "compact", "Ambient Wash", 0 } };

        for (auto& shot : shots)
        {
            ReverieProcessor p;
            prepare (p);
            for (auto& src : reverie::guide::sources)
                if (juce::String (src.name) == shot.source) reverie::ui::applyStartingPoint (p, src);
            p.apvts.state.setProperty ("ui_bottom", shot.view, nullptr);
            p.apvts.state.setProperty ("ui_width", 1280, nullptr);
            for (int i = 0; i < (int) reverie::guide::sources.size(); ++i)
                if (juce::String (reverie::guide::sources[(size_t) i].name) == shot.source) p.apvts.state.setProperty ("guide_source", i, nullptr);

            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            for (int pass = 0; pass < 8; ++pass)
            {
                auto audio = noise (2, 24000, -16.0f, 11 + pass, pass % 2 == 0 ? 9000 : 16000);
                run (p, audio, 512);
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.12, false);   // lets the editor timer run
            }

            const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            const auto file = outDir.getChildFile (juce::String ("reverie-") + shot.name + ".png");
            file.deleteFile();
            juce::FileOutputStream os (file);
            juce::PNGImageFormat png;
            check (os.openedOk() && png.writeImageToStream (img, os), "rendered " + file.getFullPathName());
        }
    }

    std::cout << (failures == 0 ? "\nALL CHECKS PASSED" : "\nFAILURES: " + juce::String (failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
