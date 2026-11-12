#pragma once

#include "PluginProcessor.h"
#include "UI/Guide.h"
#include "UI/Timeline.h"
#include "UI/Widgets.h"

class ReverieEditor final : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit ReverieEditor (ReverieProcessor&);
    ~ReverieEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    // listens to every control so the Guide can explain whatever is under the pointer
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr int baseW = 1280, fullH = 904, compactH = 696;   // with / without the bottom strip

private:
    using Section = reverie::ui::Section;
    using Knob = reverie::ui::Knob;

    /** Everything is laid out at base size inside this and scaled to the window. */
    struct Content final : juce::Component
    {
        void paint (juce::Graphics&) override;
        juce::String flowText;
    };

    /** The delay's Time control: milliseconds when free-running, a note length when synced. */
    struct TimeCell final : juce::Component
    {
        TimeCell (reverie::ui::APVTS& s, juce::Colour accent)
            : free (s, "dlyTime", "TIME", accent), synced (s, "dlyDiv", "TIME", accent)
        {
            addChildComponent (free);
            addChildComponent (synced);
        }
        void setSynced (bool on)    { free.setVisible (! on); synced.setVisible (on); }
        void resized() override     { free.setBounds (getLocalBounds()); synced.setBounds (getLocalBounds()); }
        Knob free, synced;
    };

    void timerCallback() override;
    void layoutRows();
    void updateFlowText();
    void updateHover (const juce::MouseEvent&);
    int baseH() const   { return bottomView != viewNone ? fullH : compactH; }
    void applyWindowSize (int width);
    void setBottomView (int view);

    // presets live as small XML files in ~/Library/Audio/Presets/Channel Audio/Reverie
    static juce::File presetFolder();
    void refreshPresets();
    void presetChosen();
    void savePreset();
    void loadPreset (const juce::File&);
    void resetToInit();

    Knob* knob (Section&, const char* paramId, const char* caption);
    void combo (Section&, const char* paramId, const char* caption, int width = 96);
    void headerButton (Section&, const char* paramId, const char* caption, int width);
    reverie::ui::Readout* readout (Section&, const char* caption);

    ReverieProcessor& proc;
    juce::AudioProcessorValueTreeState& apvts;
    reverie::ui::ReverieLookAndFeel lnf;
    Content content;

    juce::OwnedArray<juce::Component> owned;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachments;

    Section input, delay, ducker, reverb, output;
    reverie::ui::MeterPanel meterPanel;
    reverie::ui::GuidePanel guidePanel;
    reverie::ui::TimelinePanel timelinePanel;
    TimeCell* timeCell = nullptr;
    Knob *tempoKnob = nullptr, *characterKnob = nullptr;
    reverie::ui::Readout *timeRead = nullptr, *peakRead = nullptr;
    std::unique_ptr<reverie::ui::ChoiceButtons> routeButtons;
    std::unique_ptr<reverie::ui::LedButton> bypassButton;
    reverie::ui::TextToggle guideButton { "GUIDE", reverie::ui::col::gold }, timelineButton { "TIMELINE", reverie::ui::col::gold };
    reverie::ui::TextToggle aButton { "A", reverie::ui::col::ivory }, bButton { "B", reverie::ui::col::ivory };
    reverie::ui::TextToggle undoButton { "UNDO", reverie::ui::col::ivory }, redoButton { "REDO", reverie::ui::col::ivory };
    juce::ComboBox presetBox;
    juce::Array<juce::File> presetFiles;
    std::unique_ptr<juce::FileChooser> chooser;

    enum { viewNone = 0, viewGuide, viewTimeline };
    int bottomView = viewGuide;

    int ticks = 0, reverbType = -1;
    bool synced = false, syncKnown = false;
    float inHold[2] { -100.0f, -100.0f }, outHold[2] { -100.0f, -100.0f };
    float vuDisp = -40.0f, grDisp = 0.0f, delayHold = -100.0f, reverbHold = -100.0f, feedbackHold = 0.0f, keyHold = -100.0f;
    float peakHold = -100.0f;
    int peakHoldTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverieEditor)
};
