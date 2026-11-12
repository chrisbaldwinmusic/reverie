#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace reverie;

namespace
{
    void pushPeak (std::atomic<float>& dest, float v) noexcept
    {
        if (v > dest.load (std::memory_order_relaxed))
            dest.store (v, std::memory_order_relaxed);
    }

    float blockPeak (const float* x, int n) noexcept
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i) p = std::max (p, std::abs (x[i]));
        return p;
    }

    float levelGain (float db) noexcept     { return db <= -39.9f ? 0.0f : dbToGain (db); }
}

//==============================================================================
ReverieProcessor::ReverieProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, &undo, "REVERIE", createParameterLayout())
{
    static const char* smoothedIds[numSmoothed] =
    {
        "feedLo", "feedHi", "dlyOffset", "dlyFb", "dlyLo", "dlyHi", "dlyModRate", "dlyModDepth", "dlyChar", "dlyLevel",
        "rvPre", "rvSize", "rvDecay", "rvDamp", "rvMod", "rvLevel", "rvChar", "rvDiff", "rvLo", "rvHi", "rvWidth",
        "duckAmt", "duckThr", "duckRel", "mix"
    };

    for (int i = 0; i < numSmoothed; ++i)
    {
        smSrc[i] = apvts.getRawParameterValue (smoothedIds[i]);
        jassert (smSrc[i] != nullptr);
    }

    auto p = [this] (const char* id) { auto* v = apvts.getRawParameterValue (id); jassert (v != nullptr); return v; };
    pRoute = p ("route");       pBypass = p ("bypass");     pInGain = p ("inGain");     pOutGain = p ("outGain");
    pBpm = p ("bpm");           pDlyOn = p ("dlyOn");       pDlyType = p ("dlyType");   pDlyMode = p ("dlyMode");
    pDlySync = p ("dlySync");   pDlyTime = p ("dlyTime");   pDlyDiv = p ("dlyDiv");     pDlyOffset = p ("dlyOffset");
    pDlyFb = p ("dlyFb");       pRvOn = p ("rvOn");         pRvType = p ("rvType");     pRvDecay = p ("rvDecay");
    pRvFreeze = p ("rvFreeze"); pDuckOn = p ("duckOn");     pDuckTarget = p ("duckTarget");
    pDuckExt = p ("duckExt");   pWetOnly = p ("wetOnly");

    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
}

bool ReverieProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    const auto out = layouts.getMainOutputChannelSet();
    if ((out != mono && out != stereo) || out != layouts.getMainInputChannelSet())
        return false;

    const auto key = layouts.getChannelSet (true, 1);
    return key.isDisabled() || key == mono || key == stereo;
}

//==============================================================================
double ReverieProcessor::currentBpm() const noexcept
{
    const float host = meters.hostBpm.load (std::memory_order_relaxed);
    return host > 1.0f ? (double) host : (double) pBpm->load();
}

void ReverieProcessor::delayTimesMs (float& left, float& right) const noexcept
{
    float base = pDlyTime->load();
    if (pDlySync->load() > 0.5f)
        base = (float) (60000.0 / currentBpm()) * divisionBeats[juce::jlimit (0, numDivisions - 1, (int) pDlyDiv->load())];

    left  = juce::jlimit (1.0f, maxDelayMs, base);
    right = juce::jlimit (1.0f, maxDelayMs, base * (1.0f + 0.01f * pDlyOffset->load()));
}

double ReverieProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    if (pRvOn->load() > 0.5f)
        tail = pRvFreeze->load() > 0.5f ? 60.0 : (double) pRvDecay->load() + 0.25;

    if (pDlyOn->load() > 0.5f)
    {
        float l = 0.0f, r = 0.0f;
        delayTimesMs (l, r);
        const double fb = juce::jlimit (0.0, 0.98, 0.01 * (double) pDlyFb->load());
        const double repeats = fb > 0.001 ? std::log (0.001) / std::log (fb) : 1.0;     // repeats until 60 dB down
        tail += juce::jmin (60.0, 0.001 * (double) juce::jmax (l, r) * (repeats + 1.0));
    }
    return juce::jmin (60.0, tail);
}

//==============================================================================
void ReverieProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    chunk = 32 * juce::jmax (1, juce::roundToInt (sr / 48000.0));

    echo.prepare (sr);
    verb.prepare (sr);
    ducker.prepare (sr);
    loudness.prepare (sr);
    feedHp.reset(); feedLp.reset();

    for (auto* f : { &bypassFade, &feedHpFade, &feedLpFade, &delayFade, &reverbFade, &routeFade })
        f->prepare (sr);
    gainsPrimed = false;

    inGain.reset (sr, 0.02);
    outGain.reset (sr, 0.02);
    inGain.setCurrentAndTargetValue (dbToGain (pInGain->load()));
    outGain.setCurrentAndTargetValue (dbToGain (pOutGain->load()));

    smCoef = 1.0f - (float) std::exp (-(double) chunk / (0.008 * sr));
    smPrimed = false;

    for (auto* v : { &monoR, &dryL, &dryR, &feedL, &feedR, &tmpL, &tmpR, &sendL, &sendR, &dlyL, &dlyR, &rvL, &rvR, &zero, &duckGain })
        v->assign ((size_t) chunk, 0.0f);

    vuState = 0.0f;
    vuCoef = msToCoef (150.0f, sr);

    setLatencySamples (0);      // nothing here looks ahead or oversamples, so the dry signal is never delayed
    prepared = true;
}

//==============================================================================
void ReverieProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int mainCh = juce::jmin (getMainBusNumInputChannels(), buffer.getNumChannels());
    const int n = buffer.getNumSamples();

    if (mainCh < 1 || n == 0 || ! prepared)
        return;

    float hostBpm = 0.0f;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
            if (const auto bpm = pos->getBpm())
                hostBpm = (float) *bpm;
    meters.hostBpm.store (hostBpm, std::memory_order_relaxed);

    const bool mono = mainCh < 2;
    float* l = buffer.getWritePointer (0);
    float* r = mono ? nullptr : buffer.getWritePointer (1);

    // sidechain input, when the host has connected one: its channels follow the main input's
    const int keyCh = getBusCount (true) > 1 ? juce::jmin (getChannelCountOfBus (true, 1), buffer.getNumChannels() - mainCh) : 0;
    const float* kl = keyCh > 0 ? buffer.getReadPointer (mainCh) : nullptr;
    const float* kr = keyCh > 1 ? buffer.getReadPointer (mainCh + 1) : kl;
    meters.keyConnected.store (kl != nullptr, std::memory_order_relaxed);

    for (int pos = 0; pos < n; pos += chunk)
    {
        const int m = juce::jmin (chunk, n - pos);
        const float* ckl = kl != nullptr ? kl + pos : nullptr;
        const float* ckr = kl != nullptr ? kr + pos : nullptr;

        if (mono)
        {
            std::copy (l + pos, l + pos + m, monoR.begin());
            processChunk (l + pos, monoR.data(), ckl, ckr, m);
            for (int i = 0; i < m; ++i)     // a mono track hears both sides of the stereo effects
                l[pos + i] = 0.5f * (l[pos + i] + monoR[(size_t) i]);
        }
        else
        {
            processChunk (l + pos, r + pos, ckl, ckr, m);
        }
    }

    meters.lufsS.store (loudness.shortTerm, std::memory_order_relaxed);
    meters.lufsM.store (loudness.momentary, std::memory_order_relaxed);
}

void ReverieProcessor::processChunk (float* l, float* r, const float* kl, const float* kr, int n)
{
    const bool bypassed = pBypass->load() > 0.5f;

    for (int i = 0; i < numSmoothed; ++i)
    {
        const float target = smSrc[i]->load (std::memory_order_relaxed);
        sm[i] = smPrimed ? sm[i] + smCoef * (target - sm[i]) : target;
    }
    smPrimed = true;

    // ---- input ----------------------------------------------------------------
    inGain.setTargetValue (bypassed ? 1.0f : dbToGain (pInGain->load()));
    for (int i = 0; i < n; ++i)
    {
        const float g = inGain.getNextValue();
        l[i] *= g; r[i] *= g;
    }
    pushPeak (meters.inPeak[0], blockPeak (l, n));
    pushPeak (meters.inPeak[1], blockPeak (r, n));
    if (kl != nullptr)
        pushPeak (meters.keyPeak, std::max (blockPeak (kl, n), blockPeak (kr, n)));

    // ---- effects --------------------------------------------------------------
    // Bypass crossfades to the untouched input. Once fully bypassed the effects stop running,
    // and they start again from silence.
    bool cold = false;
    if (bypassFade.begin (! bypassed, 0, cold))
    {
        if (cold)
        {
            echo.reset(); verb.reset(); ducker.reset(); feedHp.reset(); feedLp.reset();
            gainsPrimed = false;
        }
        std::copy (l, l + n, dryL.begin());
        std::copy (r, r + n, dryR.begin());
        processEffects (l, r, kl, kr, n);
        bypassFade.mix (l, r, dryL.data(), dryR.data(), n);
    }

    // ---- output ---------------------------------------------------------------
    outGain.setTargetValue (bypassed ? 1.0f : dbToGain (pOutGain->load()));
    float pkL = 0.0f, pkR = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float g = outGain.getNextValue();
        l[i] *= g; r[i] *= g;
        pkL = std::max (pkL, std::abs (l[i]));
        pkR = std::max (pkR, std::abs (r[i]));
        vuState += vuCoef * (0.5f * (l[i] * l[i] + r[i] * r[i]) - vuState);
    }
    pushPeak (meters.outPeak[0], pkL);
    pushPeak (meters.outPeak[1], pkR);
    meters.vuRms.store (std::sqrt (vuState), std::memory_order_relaxed);
    loudness.process (l, r, n);
}

/** On entry l/r hold the dry signal (also in dryL/dryR); on exit they hold the finished mix. */
void ReverieProcessor::processEffects (float* l, float* r, const float* kl, const float* kr, int n)
{
    auto on = [] (const std::atomic<float>* p) { return p->load() > 0.5f; };
    float* const fl = feedL.data();
    float* const fr = feedR.data();

    // ---- what is sent into the effects: the dry signal, optionally filtered ----
    std::copy (l, l + n, fl);
    std::copy (r, r + n, fr);

    auto feedStage = [&] (Fade& f, bool want, Biquad& filter, auto&& design)
    {
        bool cold = false;
        if (! f.begin (want, 0, cold)) return;
        if (cold) filter.reset();

        const bool fading = ! f.steady();
        if (fading)
        {
            std::copy (fl, fl + n, tmpL.begin());
            std::copy (fr, fr + n, tmpR.begin());
        }
        design();
        for (int i = 0; i < n; ++i) { fl[i] = filter.process (0, fl[i]); fr[i] = filter.process (1, fr[i]); }
        if (fading) f.mix (fl, fr, tmpL.data(), tmpR.data(), n);
    };

    feedStage (feedHpFade, sm[sFeedLo] > 20.5f,    feedHp, [&] { feedHp.setHighPass (sr, sm[sFeedLo], 0.707); });
    feedStage (feedLpFade, sm[sFeedHi] < 19900.0f, feedLp, [&] { feedLp.setLowPass (sr, sm[sFeedHi], 0.707); });

    // ---- ducker ---------------------------------------------------------------
    const bool duckOn = on (pDuckOn);
    const bool keyed = kl != nullptr && on (pDuckExt);
    ducker.set (duckOn, sm[sDuckAmt], sm[sDuckThr], sm[sDuckRel]);
    const float gr = ducker.process (keyed ? kl : dryL.data(), keyed ? kr : dryR.data(), duckGain.data(), n);
    if (duckOn) pushPeak (meters.grDuck, gr);

    // ---- gains ramp across the chunk, so nothing zippers ------------------------
    const int target = juce::jlimit (0, 2, (int) pDuckTarget->load());
    const float mix = 0.01f * sm[sMix];
    const bool wetOnly = on (pWetOnly);

    struct Ramp { float from, step; float at (int i) const noexcept { return from + step * (float) (i + 1); } };
    auto ramp = [this, n] (float& state, float to)
    {
        if (! gainsPrimed) state = to;
        const Ramp rp { state, (to - state) / (float) n };
        state = to;
        return rp;
    };

    const float coef = smCoef;
    auto glide = [this, coef] (float state, float to) { return gainsPrimed ? state + coef * (to - state) : to; };

    const Ramp gDelay  = ramp (delayGain,  levelGain (sm[sDlyLevel]));
    const Ramp gReverb = ramp (reverbGain, levelGain (sm[sRvLevel]));
    const Ramp gDry    = ramp (dryGain,    glide (dryGain, wetOnly ? 0.0f : juce::jmin (1.0f, 2.0f * (1.0f - mix))));
    const Ramp gWet    = ramp (wetGain,    glide (wetGain, wetOnly ? 1.0f : juce::jmin (1.0f, 2.0f * mix)));
    const Ramp wDelay  = ramp (duckDelay,  glide (duckDelay,  target != 2 ? 1.0f : 0.0f));
    const Ramp wReverb = ramp (duckReverb, glide (duckReverb, target != 1 ? 1.0f : 0.0f));
    gainsPrimed = true;

    // ---- the two effects --------------------------------------------------------
    auto runDelay = [&] (const float* inL, const float* inR)
    {
        const bool want = on (pDlyOn);
        const int choice = juce::jlimit (0, 4, (int) pDlyType->load()) | (juce::jlimit (0, 2, (int) pDlyMode->load()) << 3);
        bool cold = false;

        if (! delayFade.begin (want, choice, cold))
        {
            std::fill (dlyL.begin(), dlyL.begin() + n, 0.0f);
            std::fill (dlyR.begin(), dlyR.begin() + n, 0.0f);
            return;
        }
        // A change of type or mode restarts the echoes: what is already going round the loop was
        // coloured and routed the old way, and carrying it over would put a click into every repeat.
        if (cold) echo.reset();

        float msL = 0.0f, msR = 0.0f;
        delayTimesMs (msL, msR);

        Echo::Params p;
        p.type = delayFade.active & 7;
        p.mode = delayFade.active >> 3;
        p.timeL = msL * 0.001f * (float) sr;
        p.timeR = juce::jlimit (1.0f, maxDelayMs, msL * (1.0f + 0.01f * sm[sDlyOffset])) * 0.001f * (float) sr;
        p.feedback = 0.01f * sm[sDlyFb];
        p.lowCut = sm[sDlyLo];          p.highCut = sm[sDlyHi];
        p.modRate = sm[sDlyModRate];    p.modDepth = 0.01f * sm[sDlyModDepth];
        p.character = 0.01f * sm[sDlyChar];
        echo.set (p);
        echo.process (inL, inR, dlyL.data(), dlyR.data(), n);
        pushPeak (meters.feedbackPeak, echo.feedbackPeak);

        for (int i = 0; i < n; ++i)
        {
            const float g = gDelay.at (i);
            dlyL[(size_t) i] *= g; dlyR[(size_t) i] *= g;
        }
        delayFade.mix (dlyL.data(), dlyR.data(), zero.data(), zero.data(), n);
        pushPeak (meters.delayPeak, std::max (blockPeak (dlyL.data(), n), blockPeak (dlyR.data(), n)));
    };

    auto runReverb = [&] (const float* inL, const float* inR)
    {
        bool cold = false;
        if (! reverbFade.begin (on (pRvOn), juce::jlimit (0, (int) Reverb::numTypes - 1, (int) pRvType->load()), cold))
        {
            std::fill (rvL.begin(), rvL.begin() + n, 0.0f);
            std::fill (rvR.begin(), rvR.begin() + n, 0.0f);
            return;
        }
        if (cold) verb.reset();

        Reverb::Params p;
        p.type = reverbFade.active;
        p.preMs = sm[sRvPre];           p.size = 0.01f * sm[sRvSize];
        p.decay = sm[sRvDecay];         p.dampHz = sm[sRvDamp];
        p.mod = 0.01f * sm[sRvMod];     p.character = 0.01f * sm[sRvChar];
        p.diffusion = 0.01f * sm[sRvDiff];
        p.lowCut = sm[sRvLo];           p.highCut = sm[sRvHi];
        p.width = 0.01f * sm[sRvWidth];
        p.freeze = on (pRvFreeze);
        verb.set (p);
        verb.process (inL, inR, rvL.data(), rvR.data(), n);

        for (int i = 0; i < n; ++i)
        {
            const float g = gReverb.at (i);
            rvL[(size_t) i] *= g; rvR[(size_t) i] *= g;
        }
        reverbFade.mix (rvL.data(), rvR.data(), zero.data(), zero.data(), n);
        pushPeak (meters.reverbPeak, std::max (blockPeak (rvL.data(), n), blockPeak (rvR.data(), n)));
    };

    // Changing the routing fades the effects out, swaps the order, and fades them back in.
    bool routeCold = false;
    routeFade.begin (true, juce::jlimit (0, 2, (int) pRoute->load()), routeCold);

    switch (routeFade.active)
    {
        case routeParallel:
            runDelay (fl, fr);
            runReverb (fl, fr);
            break;

        case routeReverbDelay:      // the echoes repeat the reverb
            runReverb (fl, fr);
            for (int i = 0; i < n; ++i) { sendL[(size_t) i] = fl[i] + rvL[(size_t) i]; sendR[(size_t) i] = fr[i] + rvR[(size_t) i]; }
            runDelay (sendL.data(), sendR.data());
            break;

        default:                    // each echo gets its own reverb
            runDelay (fl, fr);
            for (int i = 0; i < n; ++i) { sendL[(size_t) i] = fl[i] + dlyL[(size_t) i]; sendR[(size_t) i] = fr[i] + dlyR[(size_t) i]; }
            runReverb (sendL.data(), sendR.data());
            break;
    }

    // ---- ducking, then the wet sum ----------------------------------------------
    for (int i = 0; i < n; ++i)
    {
        const float d = duckGain[(size_t) i] - 1.0f;
        const float gd = 1.0f + wDelay.at (i) * d, gv = 1.0f + wReverb.at (i) * d;
        tmpL[(size_t) i] = dlyL[(size_t) i] * gd + rvL[(size_t) i] * gv;
        tmpR[(size_t) i] = dlyR[(size_t) i] * gd + rvR[(size_t) i] * gv;
    }
    routeFade.mix (tmpL.data(), tmpR.data(), zero.data(), zero.data(), n);

    // ---- mix --------------------------------------------------------------------
    for (int i = 0; i < n; ++i)
    {
        const float dg = gDry.at (i), wg = gWet.at (i);
        l[i] = dryL[(size_t) i] * dg + tmpL[(size_t) i] * wg;
        r[i] = dryR[(size_t) i] * dg + tmpR[(size_t) i] * wg;
    }
}

//==============================================================================
void ReverieProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = stampedState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ReverieProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        loadState (juce::ValueTree::fromXml (*xml));
}

juce::ValueTree ReverieProcessor::stampedState()
{
    auto tree = apvts.copyState();
    tree.setProperty (stateVersionId, stateVersion, nullptr);
    tree.setProperty ("pluginVersion", JucePlugin_VersionString, nullptr);     // for reading by eye only
    return tree;
}

/** Brings a saved tree written by an older Reverie up to the current format, one version at a
    time. Each step is a case that fixes up what changed in that version (a renamed id, a new
    range, a choice list that grew), so a session from any earlier version loads as it sounded. */
static void migrateState (juce::ValueTree& tree, int from)
{
    for (int v = from; v < ReverieProcessor::stateVersion; ++v)
    {
        switch (v)
        {
            // case 1: ... changes from version 1 to version 2 go here
            default: break;
        }
    }
    juce::ignoreUnused (tree);
}

bool ReverieProcessor::loadState (juce::ValueTree tree)
{
    if (! tree.isValid() || ! tree.hasType (apvts.state.getType()))
        return false;

    // States written before the version was recorded are version 1: the format has not changed since.
    // A state from a newer Reverie is loaded as far as it can be: controls this version has are
    // restored, anything it does not know about is ignored.
    const int version = (int) tree.getProperty (stateVersionId, 1);
    migrateState (tree, juce::jmin (version, stateVersion));

    // A control the saved state does not mention goes to its default, rather than keeping whatever
    // it was set to before the load, so a session or preset always sounds the same however it is opened.
    for (auto* base : getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (base))
            if (! tree.getChildWithProperty ("id", param->paramID).isValid())
            {
                juce::ValueTree child ("PARAM");
                child.setProperty ("id", param->paramID, nullptr);
                child.setProperty ("value", param->convertFrom0to1 (param->getDefaultValue()), nullptr);
                tree.appendChild (child, nullptr);
            }

    tree.setProperty (stateVersionId, stateVersion, nullptr);
    apvts.replaceState (tree);
    return true;
}

void ReverieProcessor::switchAB (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == abSlot) return;

    abState[abSlot] = apvts.copyState();
    if (abState[slot].isValid())
        apvts.replaceState (abState[slot].createCopy());    // an unused slot simply starts as a copy of the other
    abSlot = slot;
}

juce::AudioProcessorEditor* ReverieProcessor::createEditor()
{
    return new ReverieEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReverieProcessor();
}
