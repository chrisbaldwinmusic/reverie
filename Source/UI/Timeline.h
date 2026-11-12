#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace reverie::ui
{
/** A picture of what the settings will do over time, drawn from the settings themselves.

    Left: the dry hit, every echo (height = level, colour = side) and the reverb tail
    falling away, on a shared time axis. With Sync on, the grid is in beats.

    Right: the numbers behind the picture: tempo, delay times, how long the echoes and
    the tail take to die away. */
class TimelinePanel final : public juce::Component
{
public:
    explicit TimelinePanel (ReverieProcessor& p) : proc (p), state (p.apvts)
    {
        setHelpId (*this, "timeline");
    }

    /** Called by the editor's timer while the panel is showing. */
    void update()   { repaint(); }

    void paint (juce::Graphics& g) override
    {
        paintPanel (g, getLocalBounds().toFloat());
        const auto plot = plotArea().toFloat();
        g.setColour (col::well);
        g.fillRoundedRectangle (plot, 4.0f);

        // ---- settings ----
        const bool delayOn = raw ("dlyOn") > 0.5f, reverbOn = raw ("rvOn") > 0.5f, sync = raw ("dlySync") > 0.5f;
        const bool freeze = reverbOn && raw ("rvFreeze") > 0.5f;
        const int route = juce::jlimit (0, 2, (int) raw ("route")), mode = juce::jlimit (0, 2, (int) raw ("dlyMode"));
        float msL = 0.0f, msR = 0.0f;
        proc.delayTimesMs (msL, msR);
        const float tl = msL * 0.001f, tr = msR * 0.001f;
        const float fb = 0.01f * raw ("dlyFb");
        const float delayDb = raw ("dlyLevel"), reverbDb = raw ("rvLevel") - 6.0f;
        const float pre = 0.001f * raw ("rvPre"), decay = raw ("rvDecay"), build = 0.01f + 0.0004f * raw ("rvSize");
        const double bpm = proc.currentBpm();
        const float beat = (float) (60.0 / bpm);
        const bool delayAudible = delayOn && delayDb > -39.9f, reverbAudible = reverbOn && raw ("rvLevel") > -39.9f;

        // ---- time axis ----
        float want = 1.0f;
        if (delayAudible) want = juce::jmax (want, 4.5f * juce::jmax (tl, tr));
        if (reverbAudible) want = juce::jmax (want, pre + 0.75f * decay);
        span = 12.0f;
        for (float s : { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f })
            if (s >= want) { span = s; break; }

        g.setFont (font (8.5f, true));
        if (sync)
        {
            for (int b = 0; (float) b * beat <= span; ++b)
            {
                if (span / beat > 24.0f && b % 4 != 0) continue;
                const float x = xFor (plot, (float) b * beat);
                g.setColour (col::wellEdge.withAlpha (b % 4 == 0 ? 0.6f : 0.25f));
                g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
                if (b % 4 == 0 && x < plot.getRight() - 40.0f)
                {
                    g.setColour (col::creamDim.withAlpha (0.8f));
                    g.drawText ("bar " + juce::String (b / 4 + 1), juce::Rectangle<float> (x + 4.0f, plot.getBottom() - 13.0f, 50.0f, 10.0f),
                                juce::Justification::centredLeft, false);
                }
            }
        }
        else
        {
            const float step = span <= 2.0f ? 0.25f : (span <= 4.0f ? 0.5f : (span <= 8.0f ? 1.0f : 2.0f));
            for (int k = 0; (float) k * step <= span; ++k)
            {
                const float t = (float) k * step, x = xFor (plot, t);
                const bool major = std::abs (t - std::round (t)) < 1.0e-3f;
                g.setColour (col::wellEdge.withAlpha (major ? 0.55f : 0.22f));
                g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
                if (major && k > 0 && x < plot.getRight() - 40.0f)
                {
                    g.setColour (col::creamDim.withAlpha (0.8f));
                    g.drawText (juce::String (juce::roundToInt (t)) + " s", juce::Rectangle<float> (x + 4.0f, plot.getBottom() - 13.0f, 40.0f, 10.0f),
                                juce::Justification::centredLeft, false);
                }
            }
        }

        for (int db : { -12, -24, -36, -48 })
        {
            const float y = yFor (plot, (float) db);
            g.setColour (col::wellEdge.withAlpha (0.25f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (col::creamDim.withAlpha (0.7f));
            g.drawText (juce::String (db), juce::Rectangle<float> (plot.getRight() - 26.0f, y - 6.0f, 22.0f, 12.0f), juce::Justification::centredRight, false);
        }

        // ---- echoes: time, level, side ----
        struct Hit { float t, db; int side; };
        std::vector<Hit> hits;
        if (delayAudible)
        {
            const float perHop = fb > 1.0e-4f ? juce::jmin (0.0f, 20.0f * std::log10 (fb)) : -200.0f;
            auto walk = [&] (int side, bool alternate, float t0)
            {
                float t = t0, db = delayDb;
                for (int k = 0; k < 48 && t <= span && db > floorDb; ++k)
                {
                    hits.push_back ({ t, db, side });
                    if (alternate) side = 1 - side;
                    t += side == 0 ? tl : tr;
                    db += perHop;
                }
            };

            if (mode == Echo::pingPong)     walk (0, true, tl);
            else if (mode == Echo::cross)   { walk (0, true, tl); walk (1, true, tr); }
            else                            { walk (0, false, tl); walk (1, false, tr); }
        }

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());

            // ---- reverb tails ----
            auto tail = [&] (float t0, float db0, float alpha)
            {
                if (db0 <= floorDb) return;
                const float start = t0 + pre, top = start + build;
                const float end = freeze ? span + 1.0f : top + decay * (db0 - floorDb) / 60.0f;
                juce::Path p;
                p.startNewSubPath (xFor (plot, start), plot.getBottom());
                p.lineTo (xFor (plot, top), yFor (plot, db0));
                p.lineTo (xFor (plot, end), freeze ? yFor (plot, db0) : plot.getBottom());
                juce::Path fill (p);
                if (freeze) fill.lineTo (xFor (plot, end), plot.getBottom());
                fill.closeSubPath();
                g.setColour (col::blue.withAlpha (0.22f * alpha));
                g.fillPath (fill);
                g.setColour (col::blue.brighter (0.4f).withAlpha (alpha));
                g.strokePath (p, juce::PathStrokeType (1.4f));
            };

            if (reverbAudible)
            {
                if (route != routeParallel)     // each echo carries the reverb with it, or is a copy of it
                {
                    int shown = 0;
                    for (auto& h : hits)
                        if (shown++ < 8)
                            tail (h.t, reverbDb + h.db, 0.38f);
                }
                tail (0.0f, reverbDb, 1.0f);
            }

            // ---- echoes ----
            for (auto& h : hits)
            {
                const float x = xFor (plot, h.t) + (h.side == 0 ? -2.0f : 2.0f);
                g.setColour ((h.side == 0 ? col::orange : col::gold).withAlpha (0.95f));
                g.fillRoundedRectangle (x - 1.5f, yFor (plot, h.db), 3.0f, plot.getBottom() - yFor (plot, h.db), 1.0f);
            }

            // ---- the dry hit ----
            const bool dry = raw ("wetOnly") < 0.5f;
            g.setColour (col::cream.withAlpha (dry ? 0.95f : 0.25f));
            g.fillRoundedRectangle (plot.getX() + 5.0f, yFor (plot, 0.0f) + 2.0f, 4.0f, plot.getHeight() - 2.0f, 1.0f);
        }

        // ---- legend ----
        auto legend = plot.reduced (18.0f, 6.0f).removeFromTop (12.0f);
        auto key = [&] (const char* name, juce::Colour c)
        {
            g.setColour (c);
            g.fillRoundedRectangle (legend.getX(), legend.getY() + 3.0f, 8.0f, 6.0f, 1.5f);
            g.setColour (col::creamDim);
            g.setFont (font (9.0f, true, 0.1f));
            const float w = 18.0f + 6.6f * (float) std::strlen (name);
            g.drawText (name, legend.withTrimmedLeft (12.0f).withWidth (w), juce::Justification::centredLeft, false);
            legend.removeFromLeft (w + 6.0f);
        };
        key ("DRY", col::cream);
        key ("ECHO LEFT", col::orange);
        key ("ECHO RIGHT", col::gold);
        key ("REVERB", col::blue.brighter (0.4f));

        g.setColour (col::wellEdge);
        g.drawRoundedRectangle (plot, 4.0f, 1.0f);

        // ---- the numbers ----
        auto info = getLocalBounds().reduced (0, 10).removeFromRight (infoW).reduced (14, 0);
        g.setColour (col::cream);
        g.setFont (font (12.5f, true, 0.16f));
        g.drawText ("TIMELINE", info.removeFromTop (18), juce::Justification::centredLeft, false);
        g.setColour (col::gold);
        g.fillRect (info.removeFromTop (5).withTrimmedTop (3).toFloat().withHeight (1.5f));
        info.removeFromTop (6);

        auto row = [&] (const char* label, const juce::String& value)
        {
            auto r = info.removeFromTop (26);
            g.setColour (col::creamDim);
            g.setFont (font (9.0f, true, 0.12f));
            g.drawText (label, r.removeFromLeft (104), juce::Justification::centredLeft, false);
            g.setColour (col::gold.brighter (0.2f));
            g.setFont (font (12.5f, true));
            g.drawFittedText (value, r, juce::Justification::centredLeft, 1, 0.75f);
        };
        auto secs = [] (float s) { return s < 1.0f ? juce::String (juce::roundToInt (s * 1000.0f)) + " ms" : juce::String (s, 2) + " s"; };
        auto bars = [&] (float s) { return secs (s) + "  (" + juce::String (s / (4.0f * beat), 1) + " bars)"; };

        row ("TEMPO", juce::String (bpm, 1) + " BPM  " + (proc.meters.hostBpm.load() > 1.0f ? "from host" : "manual"));

        juce::String time = "off";
        if (delayOn)
        {
            time = std::abs (msL - msR) < 0.5f ? secs (tl) : "L " + secs (tl) + "  R " + secs (tr);
            if (sync)
                if (auto* div = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter ("dlyDiv")))
                    time << "  (" << div->getCurrentChoiceName() << ")";
        }
        row ("DELAY TIME", time);

        juce::String fade = "off";
        if (delayOn)
        {
            const float loop = mode == Echo::stereo ? juce::jmax (tl, tr) : 0.5f * (tl + tr);
            fade = fb >= 1.0f ? juce::String ("never: feedback is 100 % or more")
                              : (fb < 0.01f ? secs (loop) : bars (loop * (1.0f + std::log (0.001f) / std::log (fb))));
        }
        row ("ECHOES GONE BY", fade);
        row ("REVERB TAIL", ! reverbOn ? juce::String ("off") : (freeze ? juce::String ("frozen") : bars (pre + decay)));

        static const char* routes[3] = { "each echo goes into the reverb", "delay and reverb side by side", "the delay repeats the reverb" };
        row ("ROUTING", routes[route]);
    }

private:
    static constexpr int infoW = 400;
    static constexpr float floorDb = -60.0f;

    juce::Rectangle<int> plotArea() const   { return getLocalBounds().reduced (12, 12).withTrimmedRight (infoW); }
    float raw (const char* id) const        { return state.getRawParameterValue (id)->load(); }

    float xFor (juce::Rectangle<float> plot, float seconds) const
    {
        return plot.getX() + 7.0f + (plot.getWidth() - 14.0f) * seconds / span;
    }

    static float yFor (juce::Rectangle<float> plot, float db)
    {
        const float top = plot.getY() + 24.0f;      // room for the legend above 0 dB
        return top + (plot.getBottom() - top) * juce::jlimit (0.0f, 1.0f, db / floorDb);
    }

    ReverieProcessor& proc;
    APVTS& state;
    float span = 2.0f;
};
} // namespace reverie::ui
