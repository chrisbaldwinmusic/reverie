#include "PluginEditor.h"

using namespace reverie;
using namespace reverie::ui;

namespace
{
    constexpr int headerH = 74, rowH = 300, guideH = 200, margin = 10, gap = 8, meterW = 262;
    constexpr int sourceId = 200, effectId = 400, userId = 1000;     // preset menu item ids

    float toDb (float gain)     { return gain > 1.0e-5f ? 20.0f * std::log10 (gain) : -100.0f; }
}

//==============================================================================
ReverieEditor::ReverieEditor (ReverieProcessor& p)
    : AudioProcessorEditor (&p), proc (p), apvts (p.apvts),
      input  ("INPUT",  col::ivory,  2),
      delay  ("DELAY",  col::orange, 6),
      ducker ("DUCKER", col::red,    2),
      reverb ("REVERB", col::blue,   6),
      output ("OUTPUT", col::ivory,  2),
      guidePanel (p, p.apvts),
      timelinePanel (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);

    for (auto* s : { &input, &delay, &ducker, &reverb, &output })
        content.addAndMakeVisible (s);
    content.addAndMakeVisible (meterPanel);
    content.addAndMakeVisible (guidePanel);
    content.addChildComponent (timelinePanel);

    // Each module's controls are shaped by their job: a switch for a choice, a large knob for the
    // control that defines the sound, small knobs for fine-tuning, faders for levels, a drawn
    // curve for filters.
    auto segments = [this] (Section& s, const char* id, float textSize)
    {
        auto* c = new ChoiceButtons (apvts, id, s.accent);
        c->setTextSize (textSize, 0.1f);
        owned.add (c); s.addCell (c);
        return c;
    };
    auto curve = [this] (Section& s, const char* lo, const char* hi, const char* caption)
    {
        auto* c = new FilterCurve (apvts, lo, hi, caption, s.accent);
        owned.add (c); s.addCell (c);
        return c;
    };
    auto column = [] (juce::Rectangle<int>& area, int width, int gapAfter = 6)
    {
        auto c = area.removeFromLeft (width);
        area.removeFromLeft (gapAfter);
        return c;
    };

    // ---- INPUT ----
    {
        auto* gain = knob (input, "inGain", "GAIN");
        tempoField = new TempoField (apvts, col::ivory);
        owned.add (tempoField); input.addCell (tempoField);
        auto* feed = curve (input, "feedLo", "feedHi", "FEED FILTERS");
        tapButton.setClickingTogglesState (false);
        tapButton.onClick = [this] { tapTempo(); };
        setHelpId (tapButton, "tap");
        input.addHeader (&tapButton, 40);

        input.layoutBody = [=, this] (juce::Rectangle<int> b)
        {
            auto top = b.removeFromTop (112);
            gain->setBounds (top.removeFromLeft (b.getWidth() / 2).reduced (8, 2));
            tempoField->setBounds (top.withSizeKeepingCentre (top.getWidth(), 64));
            b.removeFromTop (10);
            feed->setBounds (b.withSizeKeepingCentre (b.getWidth(), juce::jmin (b.getHeight(), 128)));
        };
    }

    // ---- DELAY ----
    {
        delay.setPower (apvts, "dlyOn");
        auto* type = segments (delay, "dlyType", 11.5f);
        auto* mode = segments (delay, "dlyMode", 11.5f);
        timeCell = new TimeCell (apvts, delay.accent);
        owned.add (timeCell); delay.addCell (timeCell);
        timeRead = readout (delay, "TIME ms");
        setHelpId (*timeRead, "delay.ms");
        auto* fb      = knob (delay, "dlyFb", "FEEDBACK");
        auto* rate    = knob (delay, "dlyModRate", "MOD RATE");
        auto* depth   = knob (delay, "dlyModDepth", "MOD DEPTH");
        auto* charK   = knob (delay, "dlyChar", "CHARACTER");
        auto* offset  = knob (delay, "dlyOffset", "L/R OFFSET", Knob::bipolar);
        auto* tone    = curve (delay, "dlyLo", "dlyHi", "REPEAT FILTERS");
        headerButton (delay, "dlySync", "SYNC", 48);

        delay.layoutBody = [=, this] (juce::Rectangle<int> b)
        {
            auto strip = b.removeFromTop (30);
            type->setBounds (strip.removeFromLeft (juce::roundToInt ((float) strip.getWidth() * 0.6f)));
            strip.removeFromLeft (12);
            mode->setBounds (strip);
            b.removeFromTop (10);

            auto time = column (b, 170);
            timeRead->setBounds (time.removeFromBottom (50));
            timeCell->setBounds (time.reduced (0, 2));
            fb->setBounds (column (b, 160).withSizeKeepingCentre (160, 180));
            auto small1 = column (b, 92), small2 = column (b, 92, 10);
            rate->setBounds (small1.removeFromTop (small1.getHeight() / 2).reduced (2));
            depth->setBounds (small1.reduced (2));
            charK->setBounds (small2.removeFromTop (small2.getHeight() / 2).reduced (2));
            offset->setBounds (small2.reduced (2));
            tone->setBounds (b.withSizeKeepingCentre (b.getWidth(), juce::jmin (b.getHeight(), 150)));
        };
    }

    // ---- DUCKER ----
    {
        ducker.setPower (apvts, "duckOn");
        auto* target = segments (ducker, "duckTarget", 10.0f);
        thresholdMeter = new ThresholdMeter (apvts, ducker.accent);
        owned.add (thresholdMeter); ducker.addCell (thresholdMeter);
        auto* amount  = knob (ducker, "duckAmt", "AMOUNT");
        auto* release = knob (ducker, "duckRel", "RELEASE");
        headerButton (ducker, "duckExt", "EXT KEY", 62);
        ducker.enableGrMeter (24.0f);

        ducker.layoutBody = [=, this] (juce::Rectangle<int> b)
        {
            target->setBounds (b.removeFromTop (28));
            b.removeFromTop (8);
            thresholdMeter->setBounds (b.removeFromTop (54));
            b.removeFromTop (4);
            amount->setBounds (b.removeFromLeft (b.getWidth() / 2).reduced (4, 2));
            release->setBounds (b.reduced (4, 2));
        };
    }

    // ---- REVERB ----
    {
        reverb.setPower (apvts, "rvOn");
        auto* type = segments (reverb, "rvType", 11.5f);
        auto* freeze = new LedButton (apvts, "rvFreeze", "FREEZE", reverb.accent);
        owned.add (freeze); reverb.addCell (freeze);
        auto* size   = knob (reverb, "rvSize", "SIZE");
        auto* decay  = knob (reverb, "rvDecay", "DECAY");
        auto* pre    = knob (reverb, "rvPre", "PRE-DELAY");
        characterKnob = knob (reverb, "rvChar", "EARLY");
        auto* diff   = knob (reverb, "rvDiff", "DIFFUSION");
        auto* mod    = knob (reverb, "rvMod", "MOD");
        auto* damp   = knob (reverb, "rvDamp", "DAMPING");
        auto* width  = knob (reverb, "rvWidth", "WIDTH");
        auto* tone   = curve (reverb, "rvLo", "rvHi", "TAIL FILTERS");
        auto* charK  = characterKnob;

        reverb.layoutBody = [=] (juce::Rectangle<int> b)
        {
            auto strip = b.removeFromTop (30);
            freeze->setBounds (strip.removeFromRight (150));
            strip.removeFromRight (12);
            type->setBounds (strip);
            b.removeFromTop (10);

            size->setBounds (column (b, 140).withSizeKeepingCentre (140, 170));
            decay->setBounds (column (b, 140, 10).withSizeKeepingCentre (140, 170));
            for (auto pair : { std::pair<Knob*, Knob*> { pre, charK }, { diff, mod }, { damp, width } })
            {
                auto c = column (b, 84, pair.first == damp ? 10 : 6);
                pair.first->setBounds (c.removeFromTop (c.getHeight() / 2).reduced (2));
                pair.second->setBounds (c.reduced (2));
            }
            tone->setBounds (b.withSizeKeepingCentre (b.getWidth(), juce::jmin (b.getHeight(), 150)));
        };
    }

    // ---- OUTPUT ----  (a small mixer: the two effect levels on faders, then the blend with the dry signal)
    {
        auto* dlyFader = knob (output, "dlyLevel", "DELAY", Knob::fader);
        auto* rvFader  = knob (output, "rvLevel", "REVERB", Knob::fader);
        dlyFader->slider.setColour (juce::Slider::thumbColourId, delay.accent);
        rvFader->slider.setColour (juce::Slider::thumbColourId, reverb.accent);
        auto* mix  = knob (output, "mix", "MIX");
        auto* trim = knob (output, "outGain", "TRIM", Knob::bipolar);
        peakRead = readout (output, "PEAK dBFS");
        setHelpId (*peakRead, "meter.out");
        headerButton (output, "wetOnly", "WET ONLY", 72);

        output.layoutBody = [=, this] (juce::Rectangle<int> b)
        {
            auto faders = b.removeFromLeft (112);
            dlyFader->setBounds (faders.removeFromLeft (56).reduced (2, 4));
            rvFader->setBounds (faders.reduced (2, 4));
            b.removeFromLeft (6);
            mix->setBounds (b.removeFromTop (112).reduced (2));
            peakRead->setBounds (b.removeFromBottom (50));
            trim->setBounds (b.reduced (10, 2));
        };
    }

    // ---- header ----
    bypassButton = std::make_unique<LedButton> (apvts, "bypass", "BYPASS", col::red);
    content.addAndMakeVisible (*bypassButton);

    routeButtons = std::make_unique<ChoiceButtons> (apvts, "route", col::gold);
    routeButtons->onChange = [this] (int r) { guidePanel.setRoute (r); updateFlowText(); };
    content.addAndMakeVisible (*routeButtons);

    // The bottom strip shows the Guide, the Timeline, or folds away; the choice is saved with the session.
    bottomView = juce::jlimit (0, 2, (int) apvts.state.getProperty ("ui_bottom", (int) viewGuide));
    guideButton.onClick = [this] { setBottomView (guideButton.getToggleState() ? viewGuide : viewNone); };
    timelineButton.onClick = [this] { setBottomView (timelineButton.getToggleState() ? viewTimeline : viewNone); };
    guideButton.setToggleState (bottomView == viewGuide, juce::dontSendNotification);
    timelineButton.setToggleState (bottomView == viewTimeline, juce::dontSendNotification);

    // A/B: two complete settings snapshots to flip between
    auto showAB = [this]
    {
        aButton.setToggleState (proc.getABSlot() == 0, juce::dontSendNotification);
        bButton.setToggleState (proc.getABSlot() == 1, juce::dontSendNotification);
    };
    aButton.onClick = [this, showAB] { proc.switchAB (0); showAB(); };
    bButton.onClick = [this, showAB] { proc.switchAB (1); showAB(); };
    showAB();

    for (auto* b : { &undoButton, &redoButton }) b->setClickingTogglesState (false);
    undoButton.onClick = [this] { proc.undo.undo(); };
    redoButton.onClick = [this] { proc.undo.redo(); };

    presetBox.setTextWhenNothingSelected ("PRESETS");
    presetBox.onChange = [this] { presetChosen(); };
    refreshPresets();

    for (auto* c : std::initializer_list<juce::Component*> { &guideButton, &timelineButton, &aButton, &bButton, &undoButton, &redoButton, &presetBox })
        content.addAndMakeVisible (c);

    content.addMouseListener (this, true);
    setResizable (true, true);

    // Open at a size that fits this display, leaving room for the host's own window chrome.
    int width = (int) apvts.state.getProperty ("ui_width", 0);
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto screen = display->userBounds;
        const double fit = juce::jmin (1.0, (double) (screen.getHeight() - 110) / (double) baseH(),
                                            (double) (screen.getWidth() - 40) / (double) baseW);
        const int fitWidth = juce::roundToInt (fit * baseW);
        width = width > 0 ? juce::jmin (width, fitWidth) : fitWidth;
    }
    applyWindowSize (width > 0 ? width : baseW);

    guidePanel.setRoute (routeButtons->getIndex());
    updateFlowText();
    timerCallback();
    startTimerHz (30);
}

ReverieEditor::~ReverieEditor()
{
    stopTimer();
    content.removeMouseListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void ReverieEditor::applyWindowSize (int width)
{
    const double aspect = (double) baseW / (double) baseH();
    width = juce::jlimit (baseW / 2, baseW * 2, width);
    getConstrainer()->setFixedAspectRatio (aspect);
    setResizeLimits (baseW / 2, juce::roundToInt (baseW / 2 / aspect), baseW * 2, juce::roundToInt (baseW * 2 / aspect));
    setSize (width, juce::roundToInt (width / aspect));
    resized();
}

void ReverieEditor::setBottomView (int view)
{
    bottomView = view;
    apvts.state.setProperty ("ui_bottom", view, nullptr);
    guideButton.setToggleState (view == viewGuide, juce::dontSendNotification);
    timelineButton.setToggleState (view == viewTimeline, juce::dontSendNotification);
    applyWindowSize (getWidth());
}

//==============================================================================
juce::File ReverieEditor::presetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Audio/Presets/Channel Audio/Reverie");
}

void ReverieEditor::refreshPresets()
{
    presetFiles = presetFolder().findChildFiles (juce::File::findFiles, false, "*.reverie");
    presetFiles.sort();

    presetBox.clear (juce::dontSendNotification);
    presetBox.addItem ("Init (reset everything)", 1);

    // factory presets: the Guide's starting points, then the other jobs the unit can do
    presetBox.addSectionHeading ("Sources");
    for (int i = 0; i < (int) guide::sources.size(); ++i)
        presetBox.addItem (guide::sources[(size_t) i].name, sourceId + i);
    presetBox.addSectionHeading ("Effects");
    for (int i = 0; i < (int) guide::effects.size(); ++i)
        presetBox.addItem (guide::effects[(size_t) i].name, effectId + i);

    if (! presetFiles.isEmpty()) presetBox.addSectionHeading ("Yours");
    for (int i = 0; i < presetFiles.size(); ++i)
        presetBox.addItem (presetFiles[i].getFileNameWithoutExtension(), userId + i);
    presetBox.addSeparator();
    presetBox.addItem ("Save preset...", 2);
    presetBox.addItem ("Show presets folder", 3);
}

void ReverieEditor::presetChosen()
{
    const int id = presetBox.getSelectedId();
    if (id == 0) return;

    if (id >= userId)
    {
        loadPreset (presetFiles[id - userId]);
        return;     // leave the preset's name showing
    }
    if (id >= sourceId)
    {
        // like the Guide's button: Mix, Wet Only, the gains and the tempo are left as they are
        proc.undo.beginNewTransaction ("Preset");
        applyStartingPoint (proc, id >= effectId ? guide::effects[(size_t) (id - effectId)] : guide::sources[(size_t) (id - sourceId)]);
        return;
    }

    presetBox.setSelectedId (0, juce::dontSendNotification);
    if (id == 1) resetToInit();
    if (id == 2) savePreset();
    if (id == 3) { presetFolder().createDirectory(); presetFolder().revealToUser(); }
}

void ReverieEditor::resetToInit()
{
    proc.undo.beginNewTransaction ("Init");
    for (auto* base : proc.getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (base))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
}

/** Sets the Tempo control from the average gap between the last few clicks. */
void ReverieEditor::tapTempo()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (! taps.isEmpty() && now - taps.getLast() > 2000.0) taps.clear();
    taps.add (now);
    while (taps.size() > 6) taps.remove (0);
    if (taps.size() < 2) return;

    const double bpm = 60000.0 * (double) (taps.size() - 1) / (taps.getLast() - taps.getFirst());
    if (auto* p = apvts.getParameter ("bpm"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (40.0, 240.0, bpm)));
        p->endChangeGesture();
    }
}

void ReverieEditor::savePreset()
{
    presetFolder().createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save Reverie preset", presetFolder().getChildFile ("My Preset.reverie"), "*.reverie");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File()) return;
        file = file.withFileExtension ("reverie");

        // a preset is the sound only: window and Guide choices stay with the session
        auto tree = proc.stampedState();
        for (int i = tree.getNumProperties(); --i >= 0;)
            if (const auto name = tree.getPropertyName (i).toString(); name.startsWith ("ui_") || name.startsWith ("guide_"))
                tree.removeProperty (tree.getPropertyName (i), nullptr);

        if (auto xml = tree.createXml())
            xml->writeTo (file);

        refreshPresets();
        presetBox.setSelectedId (userId + presetFiles.indexOf (file), juce::dontSendNotification);
    });
}

void ReverieEditor::loadPreset (const juce::File& file)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;

    auto tree = juce::ValueTree::fromXml (*xml);
    for (int i = 0; i < apvts.state.getNumProperties(); ++i)    // keep this session's window and Guide choices
        if (const auto name = apvts.state.getPropertyName (i); name.toString().startsWith ("ui_") || name.toString().startsWith ("guide_"))
            tree.setProperty (name, apvts.state.getProperty (name), nullptr);

    proc.loadState (tree);
}

//==============================================================================
Knob* ReverieEditor::knob (Section& s, const char* paramId, const char* caption, Knob::Style style)
{
    auto* k = new Knob (apvts, paramId, caption, s.accent, style);
    owned.add (k);
    s.addCell (k);
    return k;
}

void ReverieEditor::headerButton (Section& s, const char* paramId, const char* caption, int width)
{
    auto* b = new LedButton (apvts, paramId, caption, s.accent);
    owned.add (b);
    s.addHeader (b, width);
}

Readout* ReverieEditor::readout (Section& s, const char* caption)
{
    auto* r = new Readout (caption, s.accent);
    owned.add (r);
    s.addCell (r);
    return r;
}

//==============================================================================
void ReverieEditor::updateHover (const juce::MouseEvent& e)
{
    if (e.eventComponent == &meterPanel)
        guidePanel.setHovered (meterPanel.helpIdAt (e.getPosition()));
    else
        guidePanel.setHovered (findHelpId (e.eventComponent));
}

void ReverieEditor::mouseEnter (const juce::MouseEvent& e)  { updateHover (e); }
void ReverieEditor::mouseMove (const juce::MouseEvent& e)   { updateHover (e); }

void ReverieEditor::mouseExit (const juce::MouseEvent& e)
{
    if (e.eventComponent == &meterPanel || findHelpId (e.eventComponent).isNotEmpty())
        guidePanel.setHovered ({});
}

//==============================================================================
void ReverieEditor::updateFlowText()
{
    const int route = routeButtons != nullptr ? routeButtons->getIndex() : 0;

    juce::StringArray flow { "IN", "FEED FILTERS" };
    if (route == routeParallel)             flow.add ("DELAY + REVERB");
    else if (route == routeReverbDelay)     { flow.add ("REVERB"); flow.add ("DELAY"); }
    else                                    { flow.add ("DELAY"); flow.add ("REVERB"); }
    flow.add ("DUCKER");
    flow.add ("MIX");
    flow.add ("OUT");

    content.flowText = flow.joinIntoString (juce::String::fromUTF8 (" \xe2\x80\xba "));
    content.repaint();
}

void ReverieEditor::layoutRows()
{
    auto layoutRow = [] (juce::Rectangle<int> area, std::initializer_list<Section*> all)
    {
        int cols = 0;
        for (auto* s : all) cols += s->preferredCols();

        const float fixed = (float) ((int) all.size() * 2 * Section::pad + ((int) all.size() - 1) * gap);
        const float cellW = ((float) area.getWidth() - fixed) / (float) cols;
        float x = (float) area.getX();

        for (auto* s : all)
        {
            const float w = cellW * (float) s->preferredCols() + 2.0f * (float) Section::pad;
            s->setBounds (juce::roundToInt (x), area.getY(), juce::roundToInt (x + w) - juce::roundToInt (x), area.getHeight());
            x += w + (float) gap;
        }
    };

    auto area = juce::Rectangle<int> (baseW, baseH()).reduced (margin);
    area.removeFromTop (headerH);

    layoutRow (area.removeFromTop (rowH), { &input, &delay, &ducker });
    area.removeFromTop (gap);

    auto rowB = area.removeFromTop (rowH);
    meterPanel.setBounds (rowB.removeFromRight (meterW));
    rowB.removeFromRight (gap);
    layoutRow (rowB, { &reverb, &output });

    guidePanel.setVisible (bottomView == viewGuide);
    timelinePanel.setVisible (bottomView == viewTimeline);
    if (bottomView != viewNone)
    {
        area.removeFromTop (gap);
        const auto strip = area.removeFromTop (guideH);
        guidePanel.setBounds (strip);
        timelinePanel.setBounds (strip);
    }
}

void ReverieEditor::resized()
{
    content.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) baseW));
    content.setBounds (0, 0, baseW, baseH());
    apvts.state.setProperty ("ui_width", getWidth(), nullptr);

    routeButtons->setBounds (baseW / 2 - 180, 10, 360, 38);
    presetBox.setBounds (286, 17, 160, 24);

    int x = baseW - margin;
    auto place = [&x] (juce::Component& c, int w, int gapAfter = 6) { x -= w; c.setBounds (x, 17, w, 24); x -= gapAfter; };
    place (*bypassButton, 70);
    place (guideButton, 54);
    place (timelineButton, 74, 12);
    place (bButton, 26, 2);
    place (aButton, 26, 12);
    place (redoButton, 44, 2);
    place (undoButton, 44);
    layoutRows();
}

void ReverieEditor::paint (juce::Graphics& g)   { g.fillAll (col::faceplate); }

void ReverieEditor::Content::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1a2435), 0.0f, 0.0f, juce::Colour (0xff0a0f18), 0.0f, r.getBottom(), false));
    g.fillAll();

    g.setColour (juce::Colours::white.withAlpha (0.012f));
    for (int y = 0; y < getHeight(); y += 3)
        g.drawHorizontalLine (y, 0.0f, r.getRight());

    g.setColour (col::cream);
    g.setFont (font (30.0f, true, 0.32f));
    g.drawText ("REVERIE", margin + 6, 12, 300, 34, juce::Justification::centredLeft, false);
    g.setColour (col::gold);
    g.setFont (font (10.0f, true, 0.34f));
    g.drawText ("BOUTIQUE DELAY & REVERB", margin + 8, 46, 300, 14, juce::Justification::centredLeft, false);

    // signal flow, under the routing selector
    g.setColour (col::creamDim);
    g.setFont (font (8.5f, true, 0.06f));
    g.drawFittedText (flowText, getWidth() / 2 - 260, 52, 520, 12, juce::Justification::centred, 1, 0.7f);

    for (auto p : { juce::Point<float> (7.0f, 7.0f), { r.getRight() - 7.0f, 7.0f }, { 7.0f, r.getBottom() - 7.0f }, { r.getRight() - 7.0f, r.getBottom() - 7.0f } })
    {
        g.setColour (juce::Colour (0xff3a4860));
        g.fillEllipse (p.x - 3.5f, p.y - 3.5f, 7.0f, 7.0f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawLine (p.x - 2.2f, p.y - 2.2f, p.x + 2.2f, p.y + 2.2f, 1.0f);
    }
}

//==============================================================================
void ReverieEditor::timerCallback()
{
    auto& m = proc.meters;
    auto take = [] (std::atomic<float>& a) { return a.exchange (0.0f, std::memory_order_relaxed); };
    auto fall = [] (float& disp, float target, float rate) { disp = target > disp ? target : juce::jmax (target, disp - rate); };
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    for (int ch = 0; ch < 2; ++ch)
    {
        fall (inHold[ch],  toDb (take (m.inPeak[ch])),  1.6f);
        fall (outHold[ch], toDb (take (m.outPeak[ch])), 1.6f);
        meterPanel.inDb[ch] = inHold[ch];
        meterPanel.outDb[ch] = outHold[ch];
    }

    const float outPk = juce::jmax (outHold[0], outHold[1]);
    if (outPk >= peakHold || ++peakHoldTicks > 60) { peakHold = outPk; peakHoldTicks = 0; }
    peakRead->set (peakHold < -90.0f ? juce::String ("-inf") : juce::String (peakHold, 1));

    fall (grDisp,       take (m.grDuck), 0.5f);
    fall (delayHold,    toDb (take (m.delayPeak)), 1.2f);
    fall (reverbHold,   toDb (take (m.reverbPeak)), 1.2f);
    fall (feedbackHold, take (m.feedbackPeak), 0.03f);
    fall (keyHold,      toDb (take (m.keyPeak)), 1.6f);
    ducker.setGr (grDisp);

    // ---- controls that change with the settings ----
    const bool nowSynced = raw ("dlySync") > 0.5f;
    if (! syncKnown || nowSynced != synced)
    {
        syncKnown = true; synced = nowSynced;
        timeCell->setSynced (synced);
    }

    float msL = 0.0f, msR = 0.0f;
    proc.delayTimesMs (msL, msR);
    timeRead->set (msL < 100.0f ? juce::String (msL, 1) : juce::String (juce::roundToInt (msL)));

    const float hostBpm = m.hostBpm.load (std::memory_order_relaxed);
    tempoField->setHostBpm (hostBpm);                            // the host's tempo wins when there is one
    tapButton.setAlpha (hostBpm > 1.0f ? 0.35f : 1.0f);

    // the threshold is set against whatever drives the ducker: the key when it is in use, otherwise the dry signal
    const bool keyed = raw ("duckExt") > 0.5f && m.keyConnected.load (std::memory_order_relaxed);
    thresholdMeter->setKeyed (keyed);
    thresholdMeter->setLevel (keyed ? keyHold : juce::jmax (inHold[0], inHold[1]));

    if (const int type = (int) raw ("rvType"); type != reverbType)
    {
        reverbType = type;
        characterKnob->setCaption (type == Reverb::spring ? "TWANG" : (type == Reverb::shimmer ? "SHIMMER" : "EARLY"));
        guidePanel.setReverbType (type);
    }

    // ---- meters ----
    const float vuTarget = juce::jlimit (-40.0f, 6.0f, toDb (m.vuRms.load (std::memory_order_relaxed)) + 3.0f + 18.0f);
    vuDisp += 0.3f * (vuTarget - vuDisp);
    meterPanel.vuDb = vuDisp;
    meterPanel.grDb = grDisp;
    meterPanel.lufsS = m.lufsS.load (std::memory_order_relaxed);
    meterPanel.lufsM = m.lufsM.load (std::memory_order_relaxed);
    meterPanel.repaint();

    if (bottomView == viewTimeline && ticks % 3 == 0)
        timelinePanel.update();

    if (++ticks % 6 == 0)
        guidePanel.refreshValue();

    // group edits into undo steps: a new step begins whenever the mouse is released
    if (ticks % 10 == 0 && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        proc.undo.beginNewTransaction();

    if (ticks % 12 == 0)
    {
        Coach::Readings r;
        r.inPeakDb = juce::jmax (inHold[0], inHold[1]);
        r.outPeakDb = peakHold;
        r.vu = vuDisp;                  r.grDuck = grDisp;
        r.delayPeakDb = delayHold;      r.reverbPeakDb = reverbHold;
        r.feedbackPeak = feedbackHold;  r.keyPeakDb = keyHold;
        r.hostBpm = hostBpm;            r.delayMs = msL;
        r.keyConnected = m.keyConnected.load (std::memory_order_relaxed);
        guidePanel.setCoach (Coach::advise (apvts, r));
    }
}
