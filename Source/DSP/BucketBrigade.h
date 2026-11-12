#pragma once

#include "Common.h"

#include <complex>

namespace reverie
{
//==============================================================================
/** One channel of a bucket-brigade delay, modelled as the circuit of a classic analog delay pedal:

        NE570 compressor > anti-aliasing filter > BBD (two MN3005s, 8192 stages) > reconstruction filter > NE570 expander

    The BBD is a chain of capacitors passing samples along at its clock rate, so it is a sampled
    system running at a rate of its own, unrelated to the host's. Following Holters and Parker
    ("A combined model for a bucket brigade device and its input and output filters", DAFx 2018),
    the two filters are kept as continuous-time systems in partial-fraction form. The input
    filter's state is read off at the exact instant of each clock tick, and each step of the BBD's
    held output is fed into the output filter at the instant it happens. That gives the real
    device's behaviour without oversampling: the clock's images, the aliasing when the clock runs
    slow, and the zero-order-hold droop.

    The delay is set the way the pedal sets it, by the clock: delay = buckets / clock rate. A delay
    time that would need the clock outside the chips' rated 10-100 kHz holds the clock at the limit
    and taps the chain earlier or later instead, as a pedal with more or fewer chips would.

    The compander halves the signal's level range in decibels before the BBD and doubles it again
    after, which is what keeps the BBD's hiss down. Its rectifiers average over about 10 ms, so a
    sudden attack is briefly over-compressed and the hiss rises and falls with the music: the
    pumping and breathing heard on the real pedals. */
struct BucketBrigade
{
    static constexpr int buckets = 4096;            // 8192 stages: two MN3005s; one sample leaves per clock cycle
    static constexpr double clockMin = 10000.0, clockMax = 100000.0;

    void prepare (double sampleRate, double maxDelaySeconds)
    {
        sr = sampleRate;
        T = 1.0 / sr;
        int n = 16;
        while (n < (int) (maxDelaySeconds * clockMin) + buckets + 16) n <<= 1;
        line.assign ((size_t) n, 0.0f);
        mask = n - 1;
        compCoef = msToCoef (10.0f, sr);           // NE570 rectifier: 10 kOhm x 1 uF
        polesReady = false;                         // the filters depend on the sample rate, so always redesign them
        setCharacter (character < 0.0f ? 0.4f : character);
        reset();
    }

    void reset() noexcept
    {
        std::fill (line.begin(), line.end(), 0.0f);
        w = 0;
        tickPhase = 0.0;
        for (auto& x : xin)  x = 0.0;
        for (auto& x : xout) x = 0.0;
        held = 0.0f;
        compEnv = expEnv = envFloor;
        noiseSeed = 0x2545f491u;
    }

    /** 0..1: lower filter cutoffs, harder drive into the chips, more charge left behind at each stage, more hiss. */
    void setCharacter (float c) noexcept
    {
        c = std::clamp (c, 0.0f, 1.0f);
        if (std::abs (c - character) < 1.0e-4f && polesReady) return;
        character = c;
        polesReady = true;

        drive = 1.0f + 2.0f * c;
        noiseLevel = dbToGain (-84.0f + 12.0f * c);

        // Charge-transfer loss: each stage passes on (1 - e) of its charge and the rest follows a
        // clock later. Over the whole chain that is close to exp (-N e (1 - cos w)) at the stage rate,
        // which the read below matches with a three-bucket smoothing around the tap (no extra delay).
        // This gentle low-pass is applied once per pass, so it is what keeps each repeat duller than
        // the one before. e runs from fresh chips (0.25e-4) to tired ones (2e-4).
        const double inefficiency = 0.25e-4 + 1.75e-4 * c;
        smear = (float) (2.0 * buckets * inefficiency / 8.0);

        // both filters are fourth-order Butterworth low-pass at the same corner, as in most pedals
        const double fc = 7000.0 - 4500.0 * c;
        const double wc = 2.0 * kPi * fc;
        std::complex<double> all[4];
        for (int k = 0; k < 4; ++k)
            all[k] = std::polar (wc, kPi * 0.5 + kPi * (2.0 * k + 1.0) / 8.0);

        for (int m = 0; m < 2; ++m)     // one pole from each conjugate pair; the real part of 2x their sum is the whole filter
        {
            std::complex<double> den = 1.0;
            for (int j = 0; j < 4; ++j)
                if (j != m) den *= all[m] - all[j];
            pole[m] = all[m];
            res[m] = 2.0 * std::pow (wc, 4.0) / den;
            decay[m] = std::exp (pole[m] * T);
        }

        // DC gains: the output filter is exact; the input filter's impulse-invariant form is
        // normalised so the whole path has unity gain at DC
        double dcIn = 0.0, dcOut = 0.0, slope = 0.0;
        for (int m = 0; m < 2; ++m)
        {
            dcIn  += (res[m] * T / (1.0 - decay[m])).real();
            dcOut += (-res[m] / pole[m]).real();
            slope += (res[m] / (pole[m] * pole[m])).real();
        }
        inGain = 1.0 / dcIn;
        outDc = dcOut;
        groupDelay = 2.0 * slope / dcOut;          // seconds, for both filters together
    }

    /** Delay the two filters and the hold add at the given delay time, in host samples. The echo
        reads that much earlier so that the repeats still land on the time set. */
    float latencySamples (float delaySamples) const noexcept
    {
        const double clock = clockFor (delaySamples);
        return (float) ((groupDelay + 0.5 / clock) * sr);
    }

    /** Advances the BBD by one host sample and returns its output. delaySamples is the total
        delay wanted, already corrected for latencySamples(). Call write() afterwards with the
        signal going in. */
    float read (float delaySamples) noexcept
    {
        delaySamples = std::max (delaySamples, 1.0f);
        const double clock = clockFor (delaySamples);
        const double ticksPerSample = clock * T;
        const double tap = std::min ((double) delaySamples * ticksPerSample, (double) (mask - 4));

        for (int m = 0; m < 2; ++m) xout[m] *= decay[m];

        tickPhase += ticksPerSample;
        while (tickPhase >= 1.0)
        {
            tickPhase -= 1.0;
            const double after = tickPhase / ticksPerSample;    // host periods from this tick to the current sample

            // input: the anti-aliasing filter's output at the tick instant, compressed signal in
            double v = 0.0;
            std::complex<double> e[2];
            for (int m = 0; m < 2; ++m)
            {
                e[m] = std::exp (pole[m] * (after * T));
                v += (res[m] * (decay[m] / e[m]) * xin[m]).real();    // e^(p * (1 - after) * T) = decay / e
            }
            float s = (float) (v * T * inGain);

            // the chips: linear at low level, then they clip, earlier on one side than the other
            // (the bucket charge runs out before the bias does); they add a little hiss of their own
            const float u = drive * s;
            s = (u >= 0.0f ? std::tanh (u) : u / std::sqrt (1.0f + 0.6f * u * u)) / drive;
            s += noiseLevel * noise();

            line[(size_t) w] = s;
            w = (w + 1) & mask;

            // output: the sample that has reached the tap end of the chain, held until the next tick
            const int ti = (int) tap;
            const float f = (float) (tap - ti);
            auto at = [this, ti, f] (int k)     // k buckets either side of the tap
            {
                const float a = line[(size_t) ((w - 1 - ti - k) & mask)], b = line[(size_t) ((w - 2 - ti - k) & mask)];
                return a + f * (b - a);
            };
            const float out = (1.0f - 2.0f * smear) * at (0) + smear * (at (-1) + at (1));
            const double step = (double) (out - held);
            held = out;
            for (int m = 0; m < 2; ++m) xout[m] += step * e[m];
        }

        double y = outDc * held;
        for (int m = 0; m < 2; ++m) y += (res[m] / pole[m] * xout[m]).real();

        // expander: gain follows the level it receives, undoing the compressor
        const float yo = (float) y;
        expEnv += compCoef * (std::abs (yo) - expEnv);
        const float expanded = yo * std::max (expEnv, envFloor) * (1.0f / refLevel);
        return rail * std::tanh (expanded * (1.0f / rail));     // the op-amp after the expander runs out of supply
    }

    /** The signal entering the delay at this host sample. */
    void write (float x) noexcept
    {
        // compressor: the gain cell sits in the op-amp's feedback, so the gain follows the output level
        const float y = x * refLevel / std::max (compEnv, envFloor);
        compEnv += compCoef * (std::abs (y) - compEnv);
        for (int m = 0; m < 2; ++m) xin[m] = decay[m] * xin[m] + (double) y;
    }

private:
    double clockFor (float delaySamples) const noexcept
    {
        return std::clamp ((double) buckets * sr / (double) delaySamples, clockMin, clockMax);
    }

    float noise() noexcept
    {
        noiseSeed ^= noiseSeed << 13; noiseSeed ^= noiseSeed >> 17; noiseSeed ^= noiseSeed << 5;
        return (float) noiseSeed * (2.0f / 4294967296.0f) - 1.0f;
    }

    // 0 dB on the compander (its unity-gain level) sits at -12 dBFS
    static constexpr float refLevel = 0.25f, envFloor = 0.25f / 31.6f, rail = 2.0f;

    double sr = 48000.0, T = 1.0 / 48000.0;
    std::vector<float> line;
    int mask = 0, w = 0;
    double tickPhase = 0.0;

    std::complex<double> pole[2], res[2], decay[2], xin[2], xout[2];
    double inGain = 1.0, outDc = 1.0, groupDelay = 0.0;
    float held = 0.0f;

    float character = -1.0f, drive = 1.0f, noiseLevel = 0.0f, smear = 0.0f;
    bool polesReady = false;
    float compEnv = 0.0f, expEnv = 0.0f, compCoef = 0.01f;
    uint32_t noiseSeed = 0x2545f491u;
};
} // namespace reverie
