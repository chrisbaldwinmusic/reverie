#pragma once

#include "Common.h"
#include "BucketBrigade.h"

namespace reverie
{
//==============================================================================
/** Circular delay line. Delays are measured back from the next sample to be written. */
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, w = 0;

    void prepare (int minLength)
    {
        int n = 16;
        while (n < minLength + 8) n <<= 1;
        buf.assign ((size_t) n, 0.0f);
        mask = n - 1;
        w = 0;
    }

    void reset() noexcept               { std::fill (buf.begin(), buf.end(), 0.0f); }
    inline void push (float x) noexcept { buf[(size_t) w] = x; w = (w + 1) & mask; }

    /** Whole-sample delay, d >= 1. */
    inline float readInt (int d) const noexcept     { return buf[(size_t) ((w - d) & mask)]; }

    /** Linear interpolation, d >= 1. Never boosts any frequency, so it is safe inside long feedback loops. */
    inline float readLinear (float d) const noexcept
    {
        const int di = (int) d;
        const float f = d - (float) di;
        const float a = buf[(size_t) ((w - di) & mask)], b = buf[(size_t) ((w - di - 1) & mask)];
        return a + f * (b - a);
    }

    /** Cubic (Catmull-Rom) interpolation, d >= 2. Keeps the top end when the delay is being modulated. */
    inline float read (float d) const noexcept
    {
        const int di = (int) d;
        const float t = d - (float) di;
        const float p0 = buf[(size_t) ((w - di + 1) & mask)], p1 = buf[(size_t) ((w - di) & mask)];
        const float p2 = buf[(size_t) ((w - di - 1) & mask)], p3 = buf[(size_t) ((w - di - 2) & mask)];
        const float c1 = 0.5f * (p2 - p0);
        const float c2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float c3 = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
        return ((c3 * t + c2) * t + c1) * t + p1;
    }
};

/** Transparent below full scale, then bends over: keeps runaway feedback finite without colouring normal levels. */
inline float softLimit (float x) noexcept
{
    const float a = std::abs (x);
    if (a <= 1.0f) return x;
    return std::copysign (1.0f + std::tanh (a - 1.0f), x);
}

//==============================================================================
/** Stereo echo. Five characters share one engine: what differs is how each repeat is
    coloured on its way round the feedback loop, so every repeat is a little more worn
    than the last, as it would be on tape or in a bucket-brigade chip.

    The output is the echoes only. */
struct Echo
{
    enum Type { digital = 0, tape, analog, lofi, reverse };
    enum Mode { stereo = 0, pingPong, cross };

    struct Params
    {
        int type = digital, mode = stereo;
        float timeL = 1000.0f, timeR = 1000.0f;     // samples
        float feedback = 0.3f;                      // 0 .. 1.1
        float lowCut = 100.0f, highCut = 8000.0f;   // Hz, inside the loop
        float modRate = 0.5f, modDepth = 0.0f;      // Hz, 0..1
        float character = 0.4f;                     // 0..1
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        maxDelay = (float) (sr * 4.0);
        for (auto& l : line) l.prepare ((int) (sr * 8.1));     // reverse playback reaches back twice the delay time
        for (auto& b : bbd) b.prepare (sr, 4.1);
        glide = 1.0f - (float) std::exp (-1.0 / (0.06 * sr));
        reset();
    }

    void reset() noexcept
    {
        for (auto& l : line) l.reset();
        for (auto& b : bbd) b.reset();
        hp.reset(); lp.reset(); bump.reset(); tone1.reset();
        primed = false;
        lfoPhase = wowPhase = flutterPhase = 0.0;
        revPhase[0] = 0.0f; revPhase[1] = 0.25f;
        held[0] = held[1] = holdCount[0] = holdCount[1] = 0.0f;
        feedbackPeak = 0.0f;
    }

    /** Call once per chunk. */
    void set (const Params& np) noexcept
    {
        p = np;
        p.timeL = std::clamp (p.timeL, 4.0f, maxDelay);
        p.timeR = std::clamp (p.timeR, 4.0f, maxDelay);
        const float c = std::clamp (p.character, 0.0f, 1.0f);

        hp.setHighPass (sr, std::max (20.0f, p.lowCut), 0.707);
        lp.setLowPass (sr, p.highCut, 0.707);

        if (p.type == tape)
        {
            bump.setPeak (sr, 95.0, 0.8, 2.0 * c);
            tone1.setCutoff (sr, 15000.0 - 10000.0 * c);
            drive = 1.0f + 3.0f * c;
        }
        else if (p.type == analog)
        {
            for (auto& b : bbd) b.setCharacter (c);
        }
        else if (p.type == lofi)
        {
            holdLen = (1.0f + 9.0f * c) * (float) (sr / 48000.0);
            quant = std::pow (2.0f, 13.0f - 8.0f * c);
        }

        // modulation is set as a pitch wobble: the same depth setting sounds equally deep at any rate
        const double twoPi = 2.0 * kPi;
        lfoInc = twoPi * p.modRate / sr;
        modSamples = p.modDepth * (float) (sr * std::min (0.005, 0.02 / (twoPi * p.modRate)));
        wowSamples = p.type == tape ? c * (float) (sr * 0.0015 / (twoPi * 0.7)) : 0.0f;
        flutterSamples = p.type == tape ? c * (float) (sr * 0.0008 / (twoPi * 6.7)) : 0.0f;
    }

    /** in and out may be the same buffers. */
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
    {
        if (! primed) { primed = true; time[0] = p.timeL; time[1] = p.timeR; }

        const double twoPi = 2.0 * kPi;
        const double wowInc = twoPi * 0.7 / sr, flutterInc = twoPi * 6.7 / sr;
        const float fb = p.feedback;
        float fbPeak = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            // a change of delay time glides, bounded so the pitch never bends more than tape being nudged
            time[0] += std::clamp (glide * (p.timeL - time[0]), -2.0f, 0.75f);
            time[1] += std::clamp (glide * (p.timeR - time[1]), -2.0f, 0.75f);

            float y[2];
            if (p.type == reverse)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float T = time[ch];
                    revPhase[ch] += 1.0f / T;
                    if (revPhase[ch] >= 1.0f) revPhase[ch] -= 1.0f;
                    const float pa = revPhase[ch], pb = pa >= 0.5f ? pa - 0.5f : pa + 0.5f;
                    const float wa = 0.5f - 0.5f * (float) std::cos (twoPi * pa);
                    y[ch] = wa * line[ch].read (2.0f + 2.0f * pa * T) + (1.0f - wa) * line[ch].read (2.0f + 2.0f * pb * T);
                }
            }
            else
            {
                float modL = 0.0f, modR = 0.0f;
                if (modSamples > 0.0f || wowSamples > 0.0f)
                {
                    lfoPhase += lfoInc;         if (lfoPhase > twoPi) lfoPhase -= twoPi;
                    wowPhase += wowInc;         if (wowPhase > twoPi) wowPhase -= twoPi;
                    flutterPhase += flutterInc; if (flutterPhase > twoPi) flutterPhase -= twoPi;
                    const float wobble = wowSamples * (float) std::sin (wowPhase) + flutterSamples * (float) std::sin (flutterPhase);
                    modL = modSamples * (float) std::sin (lfoPhase) + wobble;
                    modR = modSamples * (float) std::cos (lfoPhase) + wobble;
                }
                const float d0 = std::clamp (time[0] + std::min (modL, 0.45f * time[0]), 2.0f, maxDelay);
                const float d1 = std::clamp (time[1] + std::min (modR, 0.45f * time[1]), 2.0f, maxDelay);
                if (p.type == analog)
                {
                    // the chips' clock follows the delay time, so modulation bends the pitch as on the pedal
                    y[0] = bbd[0].read (d0 - bbd[0].latencySamples (d0));
                    y[1] = bbd[1].read (d1 - bbd[1].latencySamples (d1));
                }
                else
                {
                    y[0] = line[0].read (d0);
                    y[1] = line[1].read (d1);
                }
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                float v = lp.process (ch, hp.process (ch, y[ch]));
                switch (p.type)
                {
                    case tape:
                        v = tone1.lp (ch, bump.process (ch, v));
                        v = std::tanh (drive * v) / drive;
                        break;
                    case lofi:
                        holdCount[ch] += 1.0f;
                        if (holdCount[ch] >= holdLen) { holdCount[ch] -= holdLen; held[ch] = std::round (v * quant) / quant; }
                        v = held[ch];
                        break;
                    default: break;
                }
                y[ch] = v;
            }

            const float l = inL[i], r = inR[i];
            const float fl = softLimit (fb * y[0]), fr = softLimit (fb * y[1]);
            fbPeak = std::max (fbPeak, std::max (std::abs (fb * y[0]), std::abs (fb * y[1])));

            float in0, in1;
            switch (p.mode)
            {
                case pingPong:  in0 = 0.5f * (l + r) + fr; in1 = fl;     break;
                case cross:     in0 = l + fr;              in1 = r + fl; break;
                default:        in0 = l + fl;              in1 = r + fr; break;
            }
            if (p.type == analog) { bbd[0].write (in0); bbd[1].write (in1); }
            else                  { line[0].push (in0); line[1].push (in1); }

            outL[i] = y[0];
            outR[i] = y[1];
        }

        feedbackPeak = fbPeak;
    }

    /** Largest signal sent back round the loop in the last chunk. Above 1 the loop limiter is working. */
    float feedbackPeak = 0.0f;

private:
    Params p;
    double sr = 48000.0;
    float maxDelay = 192000.0f, glide = 0.001f;
    DelayLine line[2];
    BucketBrigade bbd[2];
    Biquad hp, lp, bump;
    OnePole tone1;
    bool primed = false;
    float time[2] { 1000.0f, 1000.0f };
    double lfoPhase = 0.0, wowPhase = 0.0, flutterPhase = 0.0, lfoInc = 0.0;
    float modSamples = 0.0f, wowSamples = 0.0f, flutterSamples = 0.0f;
    float drive = 1.0f, holdLen = 1.0f, quant = 8192.0f;
    float revPhase[2] { 0.0f, 0.25f }, held[2] {}, holdCount[2] {};
};

//==============================================================================
/** Turns the effects down while the dry signal (or an external key) is playing, and lets
    them swell back in the gaps. Writes one gain value per sample. */
struct Ducker
{
    void prepare (double sampleRate)    { sr = sampleRate; attack = msToCoef (6.0f, sr); reset(); }
    void reset() noexcept               { env = 0.0f; gain = 1.0f; }

    void set (bool isOn, float amountDb, float thresholdDb, float releaseMs) noexcept
    {
        on = isOn; amount = amountDb; threshold = thresholdDb;
        release = msToCoef (releaseMs, sr);
    }

    /** Returns the deepest gain reduction (dB, positive) in this chunk. */
    float process (const float* keyL, const float* keyR, float* gainOut, int n) noexcept
    {
        float deepest = 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const float level = std::max (std::abs (keyL[i]), std::abs (keyR[i]));
            env = level > env ? level : env + release * (level - env);

            float target = 1.0f;
            if (on && env > 1.0e-6f)
            {
                const float over = gainToDb (env) - threshold;
                if (over > 0.0f) target = dbToGain (-amount * std::min (1.0f, over / 6.0f));
            }
            gain += (target < gain ? attack : release) * (target - gain);
            gainOut[i] = gain;
            deepest = std::min (deepest, gain);
        }
        return -gainToDb (deepest);
    }

private:
    double sr = 48000.0;
    bool on = false;
    float amount = 0.0f, threshold = -30.0f, attack = 0.01f, release = 0.001f;
    float env = 0.0f, gain = 1.0f;
};
} // namespace reverie
