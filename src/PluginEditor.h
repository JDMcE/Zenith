#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_opengl/juce_opengl.h>
#include <map>
#include "PluginProcessor.h"
#include "ui/LookAndFeel.h"
#include "ui/Meters.h"
#include "ui/MainDisplay.h"
#include "ui/PresetBrowser.h"
#include "ui/HelpTexts.h"

// v0.4 editor: the real-time main display on top, knob rows below, the
// right-hand meter strip with the full loudness suite (M/S/I/LRA/TP against
// an EBU R128 or ATSC A/85 target), all inside a content component at a
// fixed logical size that the editor shell scales with an AffineTransform —
// drag any edge/corner and the whole UI scales continuously (0.5x-2x,
// aspect pinned), leaving setScaleFactor free for the host's DPI handling.
// The scale and the Compact layout option (display hidden) persist per-user
// in Zenith.settings, deliberately not in plugin state. An OpenGLContext
// composites all painting on the GPU.
class ZenithAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit ZenithAudioProcessorEditor (ZenithAudioProcessor&);
    ~ZenithAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override { g.fillAll (znth::colors::background); }
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void paintContent (juce::Graphics&);
    void layoutContent();
    void applyMeterScale();
    void setCompact (bool shouldBeCompact, bool save);
    void applyLogicalSize();   // constrainer + window size after a layout change

    // Help & accessibility (v0.7) -----------------------------------------
    void showHelpMenu();
    void openManual();
    void setHelpHints (bool on, bool save);
    void setHighContrast (bool on);
    void setTextScale (float scale);
    void applyStaticColours();   // explicit colours, re-applied on palette swap
    void updateHintBar (const juce::MouseEvent&);

    static constexpr int hintBarH = 22;

    int logicalWidth() const { return 880; }
    int logicalHeight() const
    {
        return (compact ? 396 : 580) + (helpHintsOn ? hintBarH : 0);
    }

    struct Knob
    {
        juce::Slider slider;
        juce::Label caption;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void initKnob (Knob& k, const juce::String& paramId, const juce::String& caption);
    void layoutKnob (Knob& k, juce::Rectangle<int> area);

    struct ContentComponent : juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        std::function<void (const juce::MouseEvent&)> onAnyMouseDown, onAnyMouseMove;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
        // Registered as a recursive mouse listener on itself, so these fire
        // for descendants too, with e.originalComponent set (MIDI Learn
        // menu + help hints bar).
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (onAnyMouseDown)
                onAnyMouseDown (e);
        }
        void mouseMove (const juce::MouseEvent& e) override
        {
            if (onAnyMouseMove)
                onAnyMouseMove (e);
        }
    };

    ZenithAudioProcessor& processor;
    znth::LookAndFeel lookAndFeel;
    ContentComponent content;
    juce::OpenGLContext glContext;

    // Header.
    juce::TextButton undoButton { "UNDO" }, redoButton { "REDO" };
    juce::TextButton slotAButton { "A" }, slotBButton { "B" }, copySlotButton { "COPY" };
    juce::TextButton prevPresetButton { juce::String::fromUTF8 ("\xe2\x80\xb9") },
                     nextPresetButton { juce::String::fromUTF8 ("\xe2\x80\xba") };
    juce::TextButton presetNameButton { "Init" };
    juce::TextButton compactButton { "COMPACT" };
    juce::TextButton helpButton { "HELP" };
    juce::TextButton bypassButton { "BYPASS" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    juce::Label cpuLabel;

    // Main display.
    znth::MainDisplay display { processor };

    // Controls.
    Knob gainKnob, ceilingKnob, releaseKnob, attackKnob, lookaheadKnob,
         linkTransientKnob, linkReleaseKnob;

    juce::ComboBox styleBox;
    juce::Label styleCaption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> styleAttachment;

    juce::TextButton autoReleaseButton { "AUTO REL" }, dcFilterButton { "DC" },
                     unityGainButton { "UNITY" }, auditionButton { "DELTA" },
                     extScButton { "EXT SC" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        autoReleaseAttachment, dcFilterAttachment, unityGainAttachment, auditionAttachment,
        extScAttachment;

    // Output stage row (v0.3): oversampling, true peak, dither + shaping.
    juce::ComboBox osBox, ditherBox, shapingBox;
    juce::Label outputCaption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        osAttachment, ditherAttachment, shapingAttachment;
    juce::TextButton truePeakButton { "TRUE PEAK" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> truePeakAttachment;

    // Meter strip + loudness suite. Scale/standard selections are per-user
    // view preferences persisted in Zenith.settings, never in plugin state.
    znth::LevelMeter inMeter, outMeter;
    znth::GainReductionMeter grMeter;
    juce::Label inLabel, grLabel, outLabel, grValueLabel;
    juce::Label mLabel, sLabel, iLabel, lraLabel, tpLabel;
    juce::ComboBox standardBox, meterScaleBox;
    juce::TextButton resetLoudnessButton { "RESET" };
    std::unique_ptr<juce::PropertiesFile> settings;
    float tpMaxLin = 0.0f;
    bool compact = false;

    // MIDI Learn (v0.6): right-clickable controls -> parameter id.
    std::map<juce::Component*, juce::String> midiLearnTargets;
    void showMidiLearnMenu (const juce::MouseEvent&);

    // Help hints bar (v0.7): shows the hovered control's help text.
    juce::Label hintBar;
    bool helpHintsOn = false;

    // Keyboard shortcuts overlay (F1).
    struct ShortcutsOverlay : juce::Component
    {
        ShortcutsOverlay() { setWantsKeyboardFocus (true); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override { setVisible (false); }
        bool keyPressed (const juce::KeyPress&) override { setVisible (false); return true; }
    };
    ShortcutsOverlay shortcutsOverlay;

    juce::TooltipWindow tooltipWindow { this, 700 };

    znth::PresetBrowser presetBrowser { processor.presets };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZenithAudioProcessorEditor)
};
