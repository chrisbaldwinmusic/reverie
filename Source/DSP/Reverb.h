#pragma once

#include "Delay.h"

namespace reverie
{
//==============================================================================
/** Algorithmic reverb: pre-delay, early reflections, input diffusion and an eight-line
    feedback delay network with a lossless (Hadamard) mixing matrix.

    Because the matrix loses nothing, the decay time is set entirely by the gain applied
    to each line, so Decay really is the time the tail takes to fall by 60 dB. Six types
    share the engine and differ in the size of the network, the pattern of early
    reflections, how quickly the top end dies, and two extras: a dispersive "chirp" stage
    for Spring and an octave-up feedback path for Shimmer.

    The output is the reverb only. */
struct Reverb
{
    enum Type { room = 0, chamber, hall, plate, spring, shimmer, numTypes };

    struct Params
    {
        int type = hall;
        float preMs = 20.0f, size = 0.5f, decay = 2.2f, dampHz = 6000.0f;
        float mod = 0.25f, character = 0.5f, diffusion = 0.8f;
        float lowCut = 120.0f, highCut = 12000.0f, width = 1.0f;
        bool freeze = false;
    };

    static constexpr int numLines = 8, numDiffusers = 4, numTaps = 8, springStages = 24;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& l : pre) l.prepare ((int) (sr * 0.56));
        for (auto& ch : diffuser) for (auto& l : ch) l.prepare ((int) (sr * 0.03));
        for (auto& l : line) l.prepare ((int) (sr * 0.30));
        for (auto& l : shifter) l.prepare ((int) (sr * 0.12));

        springK = std::max (1, (int) std::round (sr / 8600.0));
        for (auto& s : springState) s.assign ((size_t) (springStages * springK), 0.0f);

        slow = 1.0f - (float) std::exp (-1.0 / (0.05 * sr));
        shiftWindow = (float) (sr * 0.085);
        shiftLp.setCutoff (sr, 6500.0);
        shiftHp.setCutoff (sr, 250.0);

        for (int i = 0; i < numLines; ++i)
        {
            const double phase = 2.0 * kPi * (double) i / (double) numLines;
            lfoS[i] = (float) std::sin (phase);
            lfoC[i] = (float) std::cos (phase);
            lfoW[i] = (float) (2.0 * kPi * (0.23 + 0.11 * (double) i) / sr);
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& l : pre) l.reset();
        for (auto& ch : diffuser) for (auto& l : ch) l.reset();
        for (auto& l : line) l.reset();
        for (auto& l : shifter) l.reset();
        for (auto& s : springState) std::fill (s.begin(), s.end(), 0.0f);
        std::fill (std::begin (damp), std::end (damp), 0.0f);
        outHp.reset(); outLp.reset(); shiftLp.reset(); shiftHp.reset();
        shiftFb[0] = shiftFb[1] = 0.0f;
        shiftPhase = 0.0f; springPos = 0;
        primed = false;
    }

    /** Call once per chunk. */
    void set (const Params& np) noexcept
    {
        p = np;
        p.type = std::clamp (p.type, 0, (int) numTypes - 1);
        const auto& t = types[p.type];
        const float sizeScale = 0.5f + std::clamp (p.size, 0.0f, 1.0f);

        for (int i = 0; i < numLines; ++i)      // whole-sample lengths: a frozen tail then loses nothing at all
            lenTarget[i] = std::round (lineMs[i] * 0.001f * (float) sr * t.lenScale * sizeScale);
        tapTarget = t.erScale * sizeScale * 0.001f * (float) sr;
        preTarget = p.preMs * 0.001f * (float) sr;
        modTarget = p.freeze ? 0.0f : p.mod * t.modScale * 0.0008f * (float) sr;
        gateTarget = p.freeze ? 0.0f : 1.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int j = 0; j < numDiffusers; ++j)
                diffLen[ch][j] = std::max (1, (int) std::round (diffuserMs[ch][j] * 0.001f * (float) sr * t.apScale));
        diffGain = 0.72f * std::clamp (p.diffusion, 0.0f, 1.0f);

        earlyGain = t.early ? 0.55f * p.character : 0.0f;
        springCoef = 0.25f + 0.5f * p.character;
        shiftAmount = t.shimmer ? 0.55f * p.character : 0.0f;

        dampCoef = 1.0f - (float) std::exp (-2.0 * kPi * std::min ((double) p.dampHz, 0.45 * sr) / sr);
        outHp.setHighPass (sr, std::max (20.0f, p.lowCut), 0.707);
        outLp.setLowPass (sr, p.highCut, 0.707);

        if (! primed)
        {
            primed = true;
            std::copy (std::begin (lenTarget), std::end (lenTarget), std::begin (len));
            tap = tapTarget; preLen = preTarget; modDepth = modTarget; gate = gateTarget;
        }

        // Each line loses just enough per trip for the whole network to fall 60 dB in the decay
        // time; the top end loses more, as it does in air and on plaster.
        float total = 0.0f;
        for (int i = 0; i < numLines; ++i)
        {
            total += len[i];
            if (p.freeze) { lineGain[i] = 1.0f; hfGain[i] = 1.0f; continue; }
            lineGain[i] = std::pow (10.0f, -3.0f * len[i] / (p.decay * (float) sr));
            hfGain[i] = std::pow (lineGain[i], 1.0f / t.hfRatio - 1.0f);
        }

        // A long tail stores more energy than a short one. Half of that difference is taken
        // back out, so Decay changes the length of the tail more than its loudness.
        if (! p.freeze)
        {
            const float stored = 2.0f * (p.decay / 13.8f) * (float) sr / std::max (1.0f, total);
            lateGain = std::clamp (std::pow (0.5f / stored, 0.25f), 0.3f, 1.6f);
        }
    }

    /** in and out may be the same buffers. */
    void process (const float* inL, const float* inR, float* outL, float* outR, int n) noexcept
    {
        const auto& t = types[p.type];
        const float width = p.width;
        const double twoPi = 2.0 * kPi;

        for (int i = 0; i < n; ++i)
        {
            gate += slow * (gateTarget - gate);
            preLen += slow * (preTarget - preLen);
            tap += slow * (tapTarget - tap);
            modDepth += slow * (modTarget - modDepth);
            if (modTarget == 0.0f && modDepth < 1.0e-3f) modDepth = 0.0f;

            pre[0].push (inL[i]);
            pre[1].push (inR[i]);
            const float base = 1.0f + preLen;
            float tl = gate * pre[0].readLinear (base), tr = gate * pre[1].readLinear (base);

            // ---- early reflections: the first bounces off nearby surfaces ----
            float el = 0.0f, er = 0.0f;
            if (earlyGain > 0.0f)
            {
                for (int k = 0; k < numTaps; ++k)
                {
                    el += tapGain[k] * pre[k & 1].readLinear (base + tapMs[0][k] * tap);
                    er += tapGain[k] * pre[1 - (k & 1)].readLinear (base + tapMs[1][k] * tap);
                }
                el *= earlyGain * gate;
                er *= earlyGain * gate;
            }

            // ---- spring: a chain of stretched all-pass filters delays the highs more than the lows ----
            if (t.spring)
            {
                float* sl = springState[0].data() + springPos * springStages;
                float* sr2 = springState[1].data() + springPos * springStages;
                for (int j = 0; j < springStages; ++j)
                {
                    const float vl = tl - springCoef * sl[j]; tl = springCoef * vl + sl[j]; sl[j] = vl;
                    const float vr = tr - springCoef * sr2[j]; tr = springCoef * vr + sr2[j]; sr2[j] = vr;
                }
                if (++springPos >= springK) springPos = 0;
            }

            tl += shiftFb[0];
            tr += shiftFb[1];

            // ---- input diffusion: smears each transient into a cluster ----
            for (int j = 0; j < numDiffusers; ++j)
            {
                const float dl = diffuser[0][j].readInt (diffLen[0][j]);
                const float vl = tl - diffGain * dl;
                diffuser[0][j].push (vl);
                tl = dl + diffGain * vl;

                const float dr = diffuser[1][j].readInt (diffLen[1][j]);
                const float vr = tr - diffGain * dr;
                diffuser[1][j].push (vr);
                tr = dr + diffGain * vr;
            }

            // ---- the tank ----
            float o[numLines];
            for (int k = 0; k < numLines; ++k)
            {
                const float diff = lenTarget[k] - len[k];
                len[k] = std::abs (diff) < 1.0e-3f ? lenTarget[k] : len[k] + slow * diff;

                lfoS[k] += lfoW[k] * lfoC[k];
                lfoC[k] -= lfoW[k] * lfoS[k];

                const float x = line[k].readLinear (len[k] + modDepth * (1.0f + lfoS[k]));
                damp[k] += dampCoef * (x - damp[k]);
                o[k] = (damp[k] + hfGain[k] * (x - damp[k])) * lineGain[k];
            }

            float lateL = 0.5f * (o[0] - o[2] + o[4] - o[6]);
            float lateR = 0.5f * (o[1] - o[3] + o[5] - o[7]);

            // Hadamard mix: every line feeds every other, and no energy is lost or gained
            for (int step = 1; step < numLines; step <<= 1)
                for (int a = 0; a < numLines; a += step << 1)
                    for (int b = a; b < a + step; ++b)
                    {
                        const float u = o[b], v = o[b + step];
                        o[b] = u + v; o[b + step] = u - v;
                    }

            static constexpr float sign[numLines] = { 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f };
            for (int k = 0; k < numLines; ++k)
                line[k].push (o[k] * 0.35355339f + 0.5f * sign[k] * ((k & 1) != 0 ? tr : tl));

            // ---- shimmer: the tail, an octave up, fed back in ----
            if (shiftAmount > 0.0f)
            {
                shifter[0].push (lateL);
                shifter[1].push (lateR);
                shiftPhase += 1.0f / shiftWindow;
                if (shiftPhase >= 1.0f) shiftPhase -= 1.0f;
                const float pa = shiftPhase, pb = pa >= 0.5f ? pa - 0.5f : pa + 0.5f;
                const float wa = 0.5f - 0.5f * (float) std::cos (twoPi * pa);
                const float da = 2.0f + shiftWindow * (1.0f - pa), db = 2.0f + shiftWindow * (1.0f - pb);
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float s = wa * shifter[ch].read (da) + (1.0f - wa) * shifter[ch].read (db);
                    shiftFb[ch] = softLimit (shiftAmount * gate * shiftHp.hp (ch, shiftLp.lp (ch, s)));
                }
            }
            else
            {
                shiftFb[0] = shiftFb[1] = 0.0f;
            }

            // ---- output ----
            float l = el + lateGain * lateL, r = er + lateGain * lateR;
            l = outLp.process (0, outHp.process (0, l));
            r = outLp.process (1, outHp.process (1, r));
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * width;
            outL[i] = mid + side;
            outR[i] = mid - side;
        }
    }

private:
    struct TypeDef { float lenScale, erScale, hfRatio, apScale, modScale; bool early, spring, shimmer; };

    static constexpr TypeDef types[numTypes] =
    {
        { 0.42f, 1.00f, 0.35f, 0.7f, 0.40f, true,  false, false },   // Room
        { 0.72f, 1.50f, 0.45f, 0.9f, 0.60f, true,  false, false },   // Chamber
        { 1.30f, 2.40f, 0.40f, 1.2f, 1.00f, true,  false, false },   // Hall
        { 0.62f, 0.45f, 0.65f, 1.0f, 0.50f, true,  false, false },   // Plate
        { 0.50f, 0.00f, 0.30f, 0.6f, 0.15f, false, true,  false },   // Spring
        { 1.45f, 0.00f, 0.50f, 1.3f, 1.20f, false, false, true  },   // Shimmer
    };

    // mutually prime-ish lengths, so the echoes of one line never line up with another's
    static constexpr float lineMs[numLines] = { 31.7f, 37.3f, 43.1f, 47.9f, 55.3f, 61.7f, 71.3f, 79.9f };
    static constexpr float diffuserMs[2][numDiffusers] = { { 4.77f, 3.59f, 12.73f, 9.31f }, { 4.93f, 3.41f, 12.11f, 9.71f } };
    static constexpr float tapMs[2][numTaps] = { { 3.1f, 7.3f, 11.9f, 17.3f, 23.9f, 31.1f, 38.3f, 47.9f },
                                                 { 4.3f, 8.9f, 13.7f, 19.1f, 26.3f, 33.7f, 41.9f, 50.3f } };
    static constexpr float tapGain[numTaps] = { 1.0f, -0.8f, 0.64f, 0.51f, -0.41f, 0.33f, -0.26f, -0.21f };

    Params p;
    double sr = 48000.0;
    bool primed = false;

    DelayLine pre[2], diffuser[2][numDiffusers], line[numLines], shifter[2];
    std::vector<float> springState[2];
    int springK = 6, springPos = 0;
    Biquad outHp, outLp;
    OnePole shiftLp, shiftHp;

    float len[numLines] {}, lenTarget[numLines] {}, lineGain[numLines] {}, hfGain[numLines] {}, damp[numLines] {};
    float lfoS[numLines] {}, lfoC[numLines] {}, lfoW[numLines] {};
    int diffLen[2][numDiffusers] {};
    float diffGain = 0.5f, dampCoef = 0.5f, slow = 0.001f;
    float tap = 0.0f, tapTarget = 0.0f, preLen = 0.0f, preTarget = 0.0f;
    float modDepth = 0.0f, modTarget = 0.0f, gate = 1.0f, gateTarget = 1.0f;
    float earlyGain = 0.0f, lateGain = 1.0f, springCoef = 0.5f;
    float shiftAmount = 0.0f, shiftWindow = 4000.0f, shiftPhase = 0.0f, shiftFb[2] {};
};
} // namespace reverie
