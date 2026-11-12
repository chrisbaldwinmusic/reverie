#pragma once

#include "GuideData.h"
#include "Widgets.h"

namespace reverie::ui
{
//==============================================================================
/** Live feedback: looks at the meters and settings and says the single most useful thing. */
struct Coach
{
    enum Level { ok, note, warn };

    struct Advice
    {
        Level level = ok;
        juce::String text { "Play some audio and I'll comment on what the effects are doing." };
        bool operator!= (const Advice& o) const   { return level != o.level || text != o.text; }
    };

    struct Readings
    {
        float inPeakDb = -100, outPeakDb = -100, vu = -40, grDuck = 0;
        float delayPeakDb = -100, reverbPeakDb = -100, feedbackPeak = 0, keyPeakDb = -100;
        float hostBpm = 0, delayMs = 0;
        bool keyConnected = false;
    };

    static Advice advise (APVTS& s, const Readings& m)
    {
        auto val = [&s] (const char* id) { return s.getRawParameterValue (id)->load(); };
        auto on  = [&val] (const char* id) { return val (id) > 0.5f; };
        const int route = juce::jlimit (0, 2, (int) val ("route"));
        const bool delayOn = on ("dlyOn"), reverbOn = on ("rvOn");

        if (on ("bypass"))
            return { note, "Bypassed. Do you miss the effects, or is the track clearer without them? Match levels with Output Trim before deciding." };
        if (on ("rvFreeze") && reverbOn)
            return { note, "Freeze is holding the reverb. Nothing new is entering it; switch Freeze off and the tail will die away at the Decay setting." };
        if (m.inPeakDb < -80.0f)
            return { ok, "Waiting for audio. Play the track and I'll comment on what the effects are doing." };
        if (! delayOn && ! reverbOn)
            return { note, "Both effects are switched off, so the unit is passing the dry signal only. Use the power LEDs on Delay and Reverb." };

        if (on ("duckExt") && on ("duckOn"))
        {
            if (! m.keyConnected)
                return { warn, "External Key is on, but the host has not connected a sidechain input, so the ducker is listening to this track instead. Route a key track to the plugin's sidechain." };
            if (m.keyPeakDb < -70.0f)
                return { note, "External Key is on but nothing is arriving at the sidechain input yet. Check the key track is routed and playing." };
        }

        if (m.outPeakDb > 0.0f)
            return { warn, "Output is peaking at +" + juce::String (m.outPeakDb, 1) + " dBFS. The effects are adding level; pull Output Trim down." };

        if (delayOn && val ("dlyFb") > 100.0f)
            return { warn, "Feedback is above 100 %, so the echoes grow instead of dying away. The loop limiter is holding them; pull Feedback back when you want it to stop." };
        if (delayOn && m.feedbackPeak > 1.0f)
            return { warn, "The delay's loop limiter is working: the echoes are building up faster than they fade. Lower Feedback, or raise the Delay Low Cut to stop the low end accumulating." };

        if (delayOn && on ("dlySync") && m.hostBpm < 1.0f)
            return { note, "Sync is on but there is no host tempo, so the delay is following the Tempo knob (" + juce::String (val ("bpm"), 1) + " BPM). Set it to the tempo of the music." };

        if (route == 2 && delayOn && reverbOn && val ("dlyFb") > 55.0f && val ("rvDecay") > 4.0f)
            return { note, "Reverb into Delay with a long Decay and high Feedback builds up very quickly. Keep an eye on the output level, and filter the repeats if it turns to mud." };

        const float wetDb = juce::jmax (m.delayPeakDb, m.reverbPeakDb);
        if (! on ("wetOnly") && val ("mix") <= 50.0f && wetDb > m.inPeakDb - 1.0f && m.inPeakDb > -40.0f)
            return { note, "The effects are as loud as the dry signal. Unless that is the point, lower the Delay and Reverb Levels until the source is clearly in front." };

        if (reverbOn && val ("rvDecay") > 3.0f && val ("rvLo") < 80.0f && val ("feedLo") < 80.0f)
            return { note, "A long reverb with no low cut will cloud the bottom of the mix. Raise the Reverb Low Cut to 150 Hz or more." };
        if (reverbOn && val ("rvDecay") > 2.0f && val ("rvPre") < 5.0f && val ("rvLevel") > -14.0f)
            return { note, "A long, loud reverb with no Pre-Delay pushes the source to the back. Try 20 to 40 ms of Pre-Delay to keep it up front." };
        if (reverbOn && val ("rvWidth") > 125.0f)
            return { note, "Reverb Width is above 125 %. Check the mix in mono; an over-wide reverb can thin out or vanish." };

        if (on ("duckOn"))
        {
            if (m.grDuck < 0.3f)
                return { note, "The ducker is on but not moving. Lower its Threshold until the GR strip dips on each phrase." };
            if (val ("duckAmt") > 15.0f)
                return { note, "Ducking by more than 15 dB will be heard as pumping. That can be a great effect; for transparent clearing, 6 to 10 dB is plenty." };
        }
        else if (delayOn && val ("dlyFb") > 45.0f && val ("dlyLevel") > -12.0f && m.delayMs > 200.0f)
        {
            return { note, "A long, loud echo trail will sit on top of whatever comes next. Try the Ducker: it holds the echoes down while the source is playing." };
        }

        if (m.vu < -14.0f)
            return { note, "Level through the unit is low. Raise Input Gain towards 0 VU, particularly for the Tape and Analog delays, which respond to level." };

        static const char* fine[3] = {
            "Looks healthy. Each echo is going into the reverb. Hover any control to see what it does and what to listen for.",
            "Looks healthy. Delay and reverb are running side by side. Bypass now and then to check the effects are helping.",
            "Looks healthy. The delay is repeating the reverb tail, so listen for build-up over a whole section, not just one bar." };
        return { ok, fine[route] };
    }
};

//==============================================================================
/** Resets the effects to neutral, then applies a source's starting point.
    Bypass, input gain, output trim, mix, wet-only and the tempo are left alone. */
inline void applyStartingPoint (juce::AudioProcessor& processor, const guide::Source& source)
{
    static const juce::StringArray untouched { "bypass", "inGain", "outGain", "mix", "wetOnly", "bpm" };
    static const juce::StringArray switches { "dlyOn", "rvOn", "duckOn" };

    for (auto* base : processor.getParameters())
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (base);
        if (p == nullptr || untouched.contains (p->paramID)) continue;

        float target = p->getDefaultValue();
        if (switches.contains (p->paramID)) target = 0.0f;
        for (auto& [id, value] : source.settings)
            if (p->paramID == id) target = p->convertTo0to1 (value);

        if (std::abs (target - p->getValue()) > 1.0e-6f)
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (target);
            p->endChangeGesture();
        }
    }
}

//==============================================================================
/** The teaching strip along the bottom: choose what you're working on, get tips and a
    starting point for it, and read about whichever control is under the pointer. */
class GuidePanel final : public juce::Component
{
public:
    GuidePanel (juce::AudioProcessor& p, APVTS& s) : processor (p), state (s)
    {
        for (auto& src : guide::sources) sourceBox.addItem (src.name, sourceBox.getNumItems() + 1);
        sourceBox.setSelectedItemIndex (juce::jlimit (0, (int) guide::sources.size() - 1, (int) state.state.getProperty ("guide_source", 0)), juce::dontSendNotification);
        sourceBox.onChange = [this] { state.state.setProperty ("guide_source", sourceBox.getSelectedItemIndex(), nullptr); repaint(); };

        for (auto& st : guide::styles) styleBox.addItem (st.name, styleBox.getNumItems() + 1);
        styleBox.setSelectedItemIndex (juce::jlimit (0, (int) guide::styles.size() - 1, (int) state.state.getProperty ("guide_style", 0)), juce::dontSendNotification);
        styleBox.onChange = [this] { state.state.setProperty ("guide_style", styleBox.getSelectedItemIndex(), nullptr); repaint(); };

        apply.onClick = [this] { applyStartingPoint (processor, source()); };
        setHelpId (apply, "guide.apply");

        addAndMakeVisible (sourceBox);
        addAndMakeVisible (styleBox);
        addAndMakeVisible (apply);
    }

    void setRoute (int r)                           { r = juce::jlimit (0, 2, r); if (r != route) { route = r; repaint(); } }
    void setReverbType (int t)                      { if (t != reverbType) { reverbType = t; repaint(); } }
    void setHovered (const juce::String& id)        { if (id != hovered) { hovered = id; repaint(); } }
    void setCoach (const Coach::Advice& a)          { if (coach != a) { coach = a; repaint(); } }
    void refreshValue()                             { if (hovered.isNotEmpty()) repaint (lessonArea()); }

    void resized() override
    {
        auto left = getLocalBounds().reduced (12, 10).removeFromLeft (leftW - 24);
        left.removeFromTop (44);
        sourceBox.setBounds (left.removeFromTop (24));
        left.removeFromTop (19);
        styleBox.setBounds (left.removeFromTop (24));
        apply.setBounds (left.removeFromBottom (28));
    }

    void paint (juce::Graphics& g) override
    {
        paintPanel (g, getLocalBounds().toFloat());
        g.setColour (col::wellEdge.withAlpha (0.6f));
        g.fillRect (leftW, 12, 1, getHeight() - 24);
        g.fillRect (getWidth() - rightW, 12, 1, getHeight() - 24);

        // ---- left: what are you working on? ----
        auto left = getLocalBounds().reduced (12, 10).removeFromLeft (leftW - 24);
        g.setColour (col::cream);
        g.setFont (font (12.5f, true, 0.16f));
        g.drawText ("GUIDE", left.removeFromTop (18), juce::Justification::centredLeft, false);
        g.setColour (col::gold);
        g.fillRect (left.removeFromTop (5).withTrimmedTop (3).withHeight (2).toFloat().withHeight (1.5f));
        g.setColour (col::creamDim);
        g.setFont (font (9.0f, true, 0.12f));
        g.drawText ("SOURCE", left.removeFromTop (21).withTrimmedTop (6), juce::Justification::centredLeft, false);
        left.removeFromTop (24);
        g.drawText ("STYLE", left.removeFromTop (19).withTrimmedTop (4), juce::Justification::centredLeft, false);

        // ---- middle: tips for this source and style ----
        auto mid = getLocalBounds().reduced (0, 10).withTrimmedLeft (leftW + 14).withTrimmedRight (rightW + 14).toFloat();
        auto& src = source();
        auto& style = guide::styles[(size_t) juce::jlimit (0, (int) guide::styles.size() - 1, styleBox.getSelectedItemIndex())];

        auto head = mid.removeFromTop (20.0f);
        g.setColour (col::gold);
        g.setFont (font (12.5f, true, 0.16f));
        g.drawText (juce::String (src.name).toUpperCase(), head, juce::Justification::centredLeft, false);
        if (styleBox.getSelectedItemIndex() > 0)
        {
            g.setColour (col::creamDim);
            g.setFont (font (9.5f, true, 0.12f));
            g.drawText (juce::String (style.name).toUpperCase(), head, juce::Justification::centredRight, false);
        }
        mid.removeFromTop (3.0f);

        auto bullet = [&] (const char* text, juce::Colour textCol, juce::Colour dot)
        {
            g.setColour (dot);
            g.fillEllipse (mid.getX(), mid.getY() + 5.5f, 4.0f, 4.0f);
            const float h = drawWrapped (g, text, font (11.5f), textCol, mid.withTrimmedLeft (11.0f));
            mid.removeFromTop (h + 4.5f);
        };
        for (auto* tip : src.tips) bullet (tip, col::cream.withAlpha (0.92f), col::gold);
        if (style.note[0] != 0) bullet (style.note, col::gold.brighter (0.15f), col::cream);

        // ---- right: what does this control do? ----
        auto right = lessonArea().toFloat();
        guide::Help help = guide::routeOverview[route];
        juce::String value, title;

        if (hovered == "guide.apply")
        {
            applyHelp = "Resets both effects and dials in a sensible starting point for " + juce::String (src.name)
                      + ". Input Gain, Output Trim, Mix, Wet Only and Tempo are left alone.";
            help = { "", "Apply Starting Point", applyHelp.toRawUTF8(),
                     "It is a starting point, not a preset. The Levels depend on your track and your mix, so set those by ear once the character is right." };
        }
        else if (auto* h = guide::findHelp (hovered.toRawUTF8()))
        {
            help = *h;
            if (auto* p = state.getParameter (hovered); p != nullptr && hovered != "route")
                value = dynamic_cast<juce::AudioParameterBool*> (p) != nullptr ? (p->getValue() > 0.5f ? "ON" : "OFF")
                                                                                 : p->getCurrentValueAsText();
        }

        title = help.title;
        if (hovered == "rvChar")
        {
            static const char* names[] = { "Early Reflections", "Early Reflections", "Early Reflections", "Early Reflections", "Twang", "Shimmer" };
            title << ": " << names[juce::jlimit (0, 5, reverbType)];
        }

        auto titleRow = right.removeFromTop (18.0f);
        g.setColour (col::gold);
        g.setFont (font (11.5f, true));
        g.drawText (value, titleRow, juce::Justification::centredRight, false);
        g.setColour (col::cream);
        g.setFont (font (11.5f, true, 0.12f));
        g.drawText (title.toUpperCase(), titleRow.withTrimmedRight (value.isEmpty() ? 0.0f : 70.0f), juce::Justification::centredLeft, true);
        right.removeFromTop (3.0f);

        auto coachBox = right.removeFromBottom (48.0f);
        right.removeFromBottom (4.0f);
        right.removeFromTop (drawWrapped (g, help.what, font (11.5f), col::cream.withAlpha (0.9f), right) + 5.0f);
        drawWrapped (g, help.tip, font (11.5f).italicised(), col::gold.brighter (0.15f), right);

        // ---- live coach ----
        const auto level = coach.level == Coach::warn ? col::red : (coach.level == Coach::note ? col::gold : col::green);
        g.setColour (col::well);
        g.fillRoundedRectangle (coachBox, 4.0f);
        g.setColour (level.withAlpha (0.6f));
        g.drawRoundedRectangle (coachBox.reduced (0.5f), 4.0f, 1.0f);
        auto inner = coachBox.reduced (7.0f, 6.0f);
        auto tag = inner.removeFromLeft (34.0f).removeFromTop (14.0f);
        g.setColour (level);
        g.fillRoundedRectangle (tag, 2.0f);
        g.setColour (juce::Colour (0xff141414));
        g.setFont (font (8.5f, true, 0.12f));
        g.drawText ("LIVE", tag, juce::Justification::centred, false);
        drawWrapped (g, coach.text, font (11.0f), col::cream, inner.withTrimmedLeft (8.0f));
    }

private:
    static constexpr int leftW = 214, rightW = 430;

    juce::Rectangle<int> lessonArea() const
    {
        return getLocalBounds().reduced (0, 10).removeFromRight (rightW).reduced (14, 0);
    }

    const guide::Source& source() const
    {
        return guide::sources[(size_t) juce::jlimit (0, (int) guide::sources.size() - 1, sourceBox.getSelectedItemIndex())];
    }

    /** Gold call-to-action button. */
    struct ApplyButton final : juce::Button
    {
        ApplyButton() : juce::Button ("APPLY STARTING POINT") {}
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            const auto base = col::gold.brighter (over ? 0.2f : 0.0f).darker (down ? 0.2f : 0.0f);
            g.setGradientFill (juce::ColourGradient (base.brighter (0.25f), 0.0f, r.getY(), base.darker (0.25f), 0.0f, r.getBottom(), false));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colour (0xff15130f));
            g.setFont (font (10.5f, true, 0.12f));
            g.drawText (getName(), getLocalBounds(), juce::Justification::centred, false);
        }
    };

    juce::AudioProcessor& processor;
    APVTS& state;
    juce::ComboBox sourceBox, styleBox;
    ApplyButton apply;
    juce::String hovered, applyHelp;
    Coach::Advice coach;
    int route = 0, reverbType = 2;
};
} // namespace reverie::ui
