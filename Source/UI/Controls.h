#pragma once

#include "../DSP/Common.h"
#include "Widgets.h"

namespace reverie::ui
{
//==============================================================================
/** Base for the drawn controls bound to one parameter: gestures, reset on double-click,
    and the value shown in the caption while the pointer is over the control. */
class ParamControl : public juce::Component
{
public:
    ParamControl (APVTS& state, const juce::String& id, juce::Colour accentColour)
        : accent (accentColour), param (*state.getParameter (id)),
          attachment (param, [this] (float) { repaint(); })
    {
        setHelpId (*this, id);
        attachment.sendInitialUpdate();
    }

    void mouseEnter (const juce::MouseEvent&) override  { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        attachment.setValueAsCompleteGesture (param.convertFrom0to1 (param.getDefaultValue()));
    }

protected:
    float value() const             { return param.convertFrom0to1 (param.getValue()); }
    juce::String valueText() const  { return param.getCurrentValueAsText(); }

    void beginDrag()                { attachment.beginGesture(); dragging = true; }
    void drag (float v)             { attachment.setValueAsPartOfGesture (param.getNormalisableRange().snapToLegalValue (v)); }
    void endDrag()                  { attachment.endGesture(); dragging = false; repaint(); }

    void paintCaption (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& caption)
    {
        const bool active = isMouseOver (true) || dragging;
        g.setFont (font (10.5f, true, active ? 0.0f : 0.08f));
        g.setColour (active ? col::cream : col::creamDim);
        g.drawText (active ? valueText() : caption, area, juce::Justification::centred, false);
    }

    juce::Colour accent;
    juce::RangedAudioParameter& param;
    juce::ParameterAttachment attachment;
    bool dragging = false;
};

//==============================================================================
/** The Tempo control as a number: drag up or down to change it (Shift for fine), double-click
    to reset. Shows the host's tempo, dimmed, when the host is setting it. */
class TempoField final : public ParamControl
{
public:
    TempoField (APVTS& state, juce::Colour accentColour) : ParamControl (state, "bpm", accentColour) {}

    void setHostBpm (float bpm)     { if (std::abs (bpm - hostBpm) > 0.01f) { hostBpm = bpm; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const bool host = hostBpm > 1.0f;
        auto box = getLocalBounds().withTrimmedBottom (15).withSizeKeepingCentre (juce::jmin (112, getWidth() - 6), 40).toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (dragging || isMouseOver() ? accent.withAlpha (0.8f) : col::wellEdge);
        g.drawRoundedRectangle (box, 4.0f, 1.0f);

        g.setColour ((host ? col::creamDim : accent.brighter (0.3f)));
        g.setFont (font (20.0f, true));
        g.drawText (juce::String (host ? hostBpm : value(), 1), box.withTrimmedBottom (8.0f), juce::Justification::centred, false);
        g.setFont (font (8.0f, true, 0.2f));
        g.setColour (col::creamDim);
        g.drawText (host ? "BPM FROM HOST" : "BPM", box.withTrimmedTop (box.getHeight() - 14.0f), juce::Justification::centred, false);

        paintCaption (g, getLocalBounds().removeFromBottom (15), "TEMPO");
    }

    void mouseDown (const juce::MouseEvent&) override   { start = value(); beginDrag(); }
    void mouseUp (const juce::MouseEvent&) override     { endDrag(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float perPixel = e.mods.isShiftDown() ? 0.02f : 0.25f;
        drag (start - perPixel * (float) e.getDistanceFromDragStartY());
    }

private:
    float hostBpm = 0.0f, start = 120.0f;
};

//==============================================================================
/** Note lengths for the synced delay, laid out as music: one row per note value, shortest at the
    top, with triplet, straight and dotted side by side. */
class DivisionGrid final : public ParamControl
{
public:
    DivisionGrid (APVTS& state, juce::Colour accentColour) : ParamControl (state, "dlyDiv", accentColour)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&param))
            names = choice->choices;
    }

    void paint (juce::Graphics& g) override
    {
        const int current = juce::roundToInt (value());
        for (int i = 0; i < names.size(); ++i)
        {
            auto b = cell (i).toFloat().reduced (2.0f);
            const bool lit = i == current;
            if (lit)
            {
                g.setGradientFill (juce::ColourGradient (accent.brighter (0.25f), 0.0f, b.getY(), accent.darker (0.25f), 0.0f, b.getBottom(), false));
                g.fillRoundedRectangle (b, 3.0f);
            }
            else
            {
                g.setColour (col::control.brighter (hover == i ? 0.1f : 0.0f));
                g.fillRoundedRectangle (b, 3.0f);
                g.setColour (col::wellEdge);
                g.drawRoundedRectangle (b, 3.0f, 1.0f);
            }
            g.setColour (lit ? col::ink : col::creamDim);
            g.setFont (font (10.5f, true, 0.02f));
            g.drawText (names[i], b, juce::Justification::centred, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override     { setHover (indexAt (e.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override       { setHover (-1); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (const int i = indexAt (e.getPosition()); i >= 0)
            attachment.setValueAsCompleteGesture ((float) i);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override {}

private:
    /** Rows: 1/32 | 1/16T 1/16 1/16D | ... | 1/2T 1/2 1/2D | 1/1, with the single values in the middle column. */
    juce::Rectangle<int> cell (int i) const
    {
        const int row = i == 0 ? 0 : (i == 13 ? 5 : 1 + (i - 1) / 3);
        const int column = (i == 0 || i == 13) ? 1 : (i - 1) % 3;
        const float w = (float) getWidth() / 3.0f, h = (float) getHeight() / 6.0f;
        return juce::Rectangle<float> (w * (float) column, h * (float) row, w, h).toNearestInt();
    }

    int indexAt (juce::Point<int> p) const
    {
        for (int i = 0; i < names.size(); ++i)
            if (cell (i).contains (p)) return i;
        return -1;
    }

    void setHover (int i)   { if (i != hover) { hover = i; repaint(); } }

    juce::StringArray names;
    int hover = -1;
};

//==============================================================================
/** The Ducker's threshold, set against the level that drives it: the bar shows the dry signal
    (or the key), and the effects duck whenever it reaches past the marker. */
class ThresholdMeter final : public ParamControl
{
public:
    ThresholdMeter (APVTS& state, juce::Colour accentColour) : ParamControl (state, "duckThr", accentColour) {}

    void setLevel (float db)    { if (std::abs (db - level) > 0.1f) { level = db; repaint(); } }
    void setKeyed (bool k)      { if (k != keyed) { keyed = k; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto bar = barBounds().toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (bar, 3.0f);

        const float tx = xFor (value());
        g.setColour (accent.withAlpha (0.12f));         // where the signal makes the effects duck
        g.fillRect (juce::Rectangle<float> (tx, bar.getY(), bar.getRight() - tx, bar.getHeight()).reduced (0.0f, 1.0f));

        const float lx = xFor (level);
        auto fill = juce::Rectangle<float> (bar.getX(), bar.getY(), lx - bar.getX(), bar.getHeight()).reduced (1.0f, 5.0f);
        g.setColour ((level > value() ? accent : col::green).withAlpha (0.85f));
        g.fillRect (fill);

        g.setFont (font (8.0f, true));
        g.setColour (col::creamDim.withAlpha (0.7f));
        for (int db : { -48, -36, -24, -12 })
            g.fillRect (xFor ((float) db) - 0.5f, bar.getBottom() - 4.0f, 1.0f, 3.0f);

        // the marker
        g.setColour (col::cream);
        g.fillRect (tx - 1.0f, bar.getY() - 3.0f, 2.0f, bar.getHeight() + 6.0f);
        juce::Path tri;
        tri.addTriangle (tx - 5.0f, bar.getY() - 7.0f, tx + 5.0f, bar.getY() - 7.0f, tx, bar.getY() - 1.0f);
        g.fillPath (tri);

        g.setColour (col::creamDim);
        g.drawText (keyed ? "KEY" : "DRY", bar.reduced (6.0f, 0.0f), juce::Justification::centredLeft, false);
        g.setColour (col::wellEdge);
        g.drawRoundedRectangle (bar, 3.0f, 1.0f);

        paintCaption (g, getLocalBounds().removeFromBottom (15), "THRESHOLD");
    }

    void mouseDown (const juce::MouseEvent& e) override { beginDrag(); drag (dbFor ((float) e.x)); }
    void mouseDrag (const juce::MouseEvent& e) override { drag (dbFor ((float) e.x)); }
    void mouseUp (const juce::MouseEvent&) override     { endDrag(); }

private:
    static constexpr float lo = -60.0f, hi = 0.0f;
    juce::Rectangle<int> barBounds() const  { return getLocalBounds().withTrimmedBottom (17).withTrimmedTop (8).reduced (4, 0); }
    float xFor (float db) const
    {
        const auto b = barBounds().toFloat();
        return b.getX() + b.getWidth() * juce::jlimit (0.0f, 1.0f, (db - lo) / (hi - lo));
    }
    float dbFor (float x) const
    {
        const auto b = barBounds().toFloat();
        return lo + (hi - lo) * juce::jlimit (0.0f, 1.0f, (x - b.getX()) / b.getWidth());
    }

    float level = -100.0f;
    bool keyed = false;
};

//==============================================================================
/** A pair of cut filters drawn as their actual response on a log frequency axis, with a handle
    for each corner. Drag a handle sideways; double-click a handle to switch that filter off. */
class FilterCurve final : public juce::Component
{
public:
    FilterCurve (APVTS& state, const juce::String& lowId, const juce::String& highId,
                 const juce::String& captionText, juce::Colour accentColour)
        : caption (captionText), accent (accentColour),
          lowParam (*state.getParameter (lowId)), highParam (*state.getParameter (highId)),
          lowAttach (lowParam, [this] (float) { repaint(); }),
          highAttach (highParam, [this] (float) { repaint(); })
    {
        setHelpId (*this, lowId);
        lowAttach.sendInitialUpdate();
        highAttach.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        auto plot = plotBounds().toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (plot, 3.0f);

        g.setColour (col::wellEdge.withAlpha (0.45f));
        for (float f : { 100.0f, 1000.0f, 10000.0f })
            g.fillRect (xFor (f) - 0.5f, plot.getY() + 2.0f, 1.0f, plot.getHeight() - 4.0f);
        g.setFont (font (7.5f, true));
        g.setColour (col::creamDim.withAlpha (0.6f));
        g.drawText ("100", juce::Rectangle<float> (xFor (100.0f) + 2.0f, plot.getBottom() - 11.0f, 30.0f, 10.0f), juce::Justification::left, false);
        g.drawText ("1k",  juce::Rectangle<float> (xFor (1000.0f) + 2.0f, plot.getBottom() - 11.0f, 30.0f, 10.0f), juce::Justification::left, false);
        g.drawText ("10k", juce::Rectangle<float> (xFor (10000.0f) + 2.0f, plot.getBottom() - 11.0f, 30.0f, 10.0f), juce::Justification::left, false);

        // the response, from the same filter designs the audio uses
        const double fs = 96000.0;
        Biquad hp, lp;
        const bool hpOn = low() > 20.5f, lpOn = high() < 19900.0f;
        hp.setHighPass (fs, low(), 0.707);
        lp.setLowPass (fs, high(), 0.707);

        juce::Path curve;
        bool first = true;
        const float top = plot.getY() + 10.0f, span = plot.getHeight() - 22.0f;    // 0 dB at the top, -24 dB at the bottom
        for (float x = plot.getX(); x <= plot.getRight(); x += 1.5f)
        {
            const double w = 2.0 * kPi * freqFor (x) / fs;
            double mag = 1.0;
            if (hpOn) mag *= hp.magnitude (w);
            if (lpOn) mag *= lp.magnitude (w);
            const float db = juce::jmax (-24.0f, (float) (20.0 * std::log10 (std::max (mag, 1.0e-6))));
            const float y = top - db / 24.0f * span;
            if (first) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
            first = false;
        }

        juce::Path fill (curve);
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.lineTo (plot.getX(), plot.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.28f), 0.0f, top, accent.withAlpha (0.02f), 0.0f, plot.getBottom(), false));
        g.fillPath (fill);
        g.setColour (accent.brighter (0.2f));
        g.strokePath (curve, juce::PathStrokeType (1.6f));

        for (int h = 0; h < 2; ++h)
        {
            const bool on = h == 0 ? hpOn : lpOn;
            const auto c = handleCentre (h);
            const bool hot = active == h || (active < 0 && hover == h);
            g.setColour (on ? accent.brighter (hot ? 0.5f : 0.1f) : col::ledOff.brighter (hot ? 0.6f : 0.3f));
            g.fillEllipse (juce::Rectangle<float> (11.0f, 11.0f).withCentre (c));
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.drawEllipse (juce::Rectangle<float> (11.0f, 11.0f).withCentre (c), 1.0f);
        }

        g.setColour (col::wellEdge);
        g.drawRoundedRectangle (plot, 3.0f, 1.0f);

        // caption: the handle's value while it is pointed at or dragged
        const int shown = active >= 0 ? active : hover;
        g.setFont (font (10.5f, true, shown >= 0 ? 0.0f : 0.08f));
        g.setColour (shown >= 0 ? col::cream : col::creamDim);
        g.drawText (shown == 0 ? "LOW CUT " + lowParam.getCurrentValueAsText()
                  : shown == 1 ? "HIGH CUT " + highParam.getCurrentValueAsText() : caption,
                    getLocalBounds().removeFromBottom (15), juce::Justification::centred, false);
    }

    void mouseMove (const juce::MouseEvent& e) override     { setHover (nearest (e.position)); }
    void mouseExit (const juce::MouseEvent&) override       { setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        active = nearest (e.position);
        attach (active).beginGesture();
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (active < 0) return;
        auto& p = paramFor (active);
        attach (active).setValueAsPartOfGesture (p.getNormalisableRange().snapToLegalValue (
            juce::jlimit (p.getNormalisableRange().start, p.getNormalisableRange().end, freqFor (e.position.x))));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (active >= 0) attach (active).endGesture();
        active = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = nearest (e.position);
        auto& p = paramFor (h);
        attach (h).setValueAsCompleteGesture (h == 0 ? p.getNormalisableRange().start : p.getNormalisableRange().end);   // off
    }

private:
    float low() const   { return lowParam.convertFrom0to1 (lowParam.getValue()); }
    float high() const  { return highParam.convertFrom0to1 (highParam.getValue()); }
    juce::RangedAudioParameter& paramFor (int h)    { return h == 0 ? lowParam : highParam; }
    juce::ParameterAttachment& attach (int h)       { return h == 0 ? lowAttach : highAttach; }

    juce::Rectangle<int> plotBounds() const     { return getLocalBounds().withTrimmedBottom (17).reduced (2, 2); }

    float xFor (float f) const
    {
        const auto b = plotBounds().toFloat().reduced (6.0f, 0.0f);
        return b.getX() + b.getWidth() * std::log (f / 20.0f) / std::log (1000.0f);
    }
    float freqFor (float x) const
    {
        const auto b = plotBounds().toFloat().reduced (6.0f, 0.0f);
        return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - b.getX()) / b.getWidth()));
    }

    juce::Point<float> handleCentre (int h) const
    {
        const auto b = plotBounds().toFloat();
        return { xFor (h == 0 ? low() : high()), b.getY() + 10.0f + 3.0f / 24.0f * (b.getHeight() - 22.0f) };    // at the -3 dB point
    }

    int nearest (juce::Point<float> p) const
    {
        return p.getDistanceFrom (handleCentre (0)) <= p.getDistanceFrom (handleCentre (1)) ? 0 : 1;
    }

    void setHover (int h)
    {
        if (h == hover) return;
        hover = h;
        setHelpId (*this, (h == 1 ? highParam : lowParam).paramID);     // the Guide explains the handle under the pointer
        repaint();
    }

    juce::String caption;
    juce::Colour accent;
    juce::RangedAudioParameter &lowParam, &highParam;
    juce::ParameterAttachment lowAttach, highAttach;
    int hover = -1, active = -1;
};
} // namespace reverie::ui
