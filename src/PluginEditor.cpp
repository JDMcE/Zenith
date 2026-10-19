#include "PluginEditor.h"

using namespace znth;

ZenithAudioProcessorEditor::ZenithAudioProcessorEditor (ZenithAudioProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (true);

    content.onPaint = [this] (juce::Graphics& g) { paintContent (g); };
    content.onAnyMouseDown = [this] (const juce::MouseEvent& e) { showMidiLearnMenu (e); };
    content.onAnyMouseMove = [this] (const juce::MouseEvent& e) { updateHintBar (e); };
    content.addMouseListener (&content, true); // descendants too (MIDI Learn + hints)
    addAndMakeVisible (content);

    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Zenith";
        o.folderName = "Equinox Audio";
        o.filenameSuffix = "settings";
        o.osxLibrarySubFolder = "Application Support";
        settings = std::make_unique<juce::PropertiesFile> (o);
    }

    // Accessibility preferences land before any control reads the palette.
    jdm::colors::setHighContrast (settings->getBoolValue ("high_contrast", false));
    lookAndFeel.applyPalette();
    lookAndFeel.setTextScale ((float) settings->getDoubleValue ("text_scale", 1.0));
    helpHintsOn = settings->getBoolValue ("help_hints", false);

    // Header ---------------------------------------------------------------
    undoButton.onClick = [this] { processor.stateManager.undo(); };
    redoButton.onClick = [this] { processor.stateManager.redo(); };
    undoButton.setTooltip (help::undo);
    redoButton.setTooltip (help::redo);

    slotAButton.onClick = [this] { processor.stateManager.setActiveSlot (0); };
    slotBButton.onClick = [this] { processor.stateManager.setActiveSlot (1); };
    copySlotButton.onClick = [this] { processor.stateManager.copyActiveToOther(); };
    slotAButton.setTooltip (help::slotA);
    slotBButton.setTooltip (help::slotB);
    copySlotButton.setTooltip (help::copySlot);

    prevPresetButton.onClick = [this] { processor.presets.loadAdjacent (false); };
    nextPresetButton.onClick = [this] { processor.presets.loadAdjacent (true); };
    presetNameButton.onClick = [this] { presetBrowser.openBrowser(); };
    prevPresetButton.setTooltip (help::prevPreset);
    nextPresetButton.setTooltip (help::nextPreset);
    presetNameButton.setTooltip (help::presetName);

    compactButton.setClickingTogglesState (true);
    compactButton.onClick = [this] { setCompact (compactButton.getToggleState(), true); };
    compactButton.setTooltip (help::compact);

    helpButton.onClick = [this] { showHelpMenu(); };
    helpButton.setTooltip (help::helpButton);

    bypassButton.setClickingTogglesState (true);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.apvts, id::bypass, bypassButton);
    bypassButton.setTooltip (help::bypass);

    for (auto* b : { &undoButton, &redoButton, &slotAButton, &slotBButton, &copySlotButton,
                     &prevPresetButton, &nextPresetButton, &presetNameButton,
                     &compactButton, &helpButton, &bypassButton })
    {
        b->setTitle (b->getButtonText());
        content.addAndMakeVisible (*b);
    }
    cpuLabel.setTooltip (help::cpu);

    cpuLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    cpuLabel.setColour (juce::Label::textColourId, colors::textDim);
    cpuLabel.setJustificationType (juce::Justification::centredRight);
    content.addAndMakeVisible (cpuLabel);

    // Main display ---------------------------------------------------------
    content.addAndMakeVisible (display);

    // Knobs ----------------------------------------------------------------
    initKnob (gainKnob, id::gain, "GAIN");
    initKnob (ceilingKnob, id::ceiling, "CEILING");
    initKnob (releaseKnob, id::release, "RELEASE");
    initKnob (attackKnob, id::attack, "ATTACK");
    initKnob (lookaheadKnob, id::lookahead, "LOOKAHEAD");
    initKnob (linkTransientKnob, id::linkTransient, "TRANS LINK");
    initKnob (linkReleaseKnob, id::linkRelease, "REL LINK");

    gainKnob.slider.setTooltip (help::gain);
    ceilingKnob.slider.setTooltip (help::ceiling);
    releaseKnob.slider.setTooltip (help::release);
    attackKnob.slider.setTooltip (help::attack);
    lookaheadKnob.slider.setTooltip (help::lookahead);
    linkTransientKnob.slider.setTooltip (help::linkTransient);
    linkReleaseKnob.slider.setTooltip (help::linkRelease);

    // Style + toggles ------------------------------------------------------
    styleBox.addItemList (styleChoices, 1);
    styleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.apvts, id::style, styleBox);
    styleBox.setTooltip (help::styleBox);
    styleBox.setTitle ("Style");
    content.addAndMakeVisible (styleBox);

    styleCaption.setText ("STYLE", juce::dontSendNotification);
    styleCaption.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    styleCaption.setColour (juce::Label::textColourId, colors::textDim);
    styleCaption.setJustificationType (juce::Justification::centredLeft);
    content.addAndMakeVisible (styleCaption);

    auto initToggle = [this] (juce::TextButton& b, const juce::String& paramId,
                              const juce::String& tip,
                              std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& att)
    {
        b.setClickingTogglesState (true);
        att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            processor.apvts, paramId, b);
        b.setTooltip (tip);
        b.setTitle (b.getButtonText());
        content.addAndMakeVisible (b);
    };
    initToggle (autoReleaseButton, id::autoRelease, help::autoRelease, autoReleaseAttachment);
    initToggle (dcFilterButton, id::dcFilter, help::dcFilter, dcFilterAttachment);
    initToggle (unityGainButton, id::unityGain, help::unityGain, unityGainAttachment);
    initToggle (auditionButton, id::audition, help::audition, auditionAttachment);
    initToggle (extScButton, id::extSidechain, help::extSidechain, extScAttachment);

    // Output stage row -----------------------------------------------------
    outputCaption.setText ("OUTPUT", juce::dontSendNotification);
    outputCaption.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    outputCaption.setColour (juce::Label::textColourId, colors::textDim);
    outputCaption.setJustificationType (juce::Justification::centredLeft);
    content.addAndMakeVisible (outputCaption);

    osBox.addItemList (oversamplingChoices, 1);
    osAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.apvts, id::oversampling, osBox);
    osBox.setTooltip (help::oversampling);
    osBox.setTitle ("Oversampling");
    content.addAndMakeVisible (osBox);

    truePeakButton.setClickingTogglesState (true);
    truePeakAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.apvts, id::truePeak, truePeakButton);
    truePeakButton.setTooltip (help::truePeak);
    truePeakButton.setTitle ("True Peak");
    content.addAndMakeVisible (truePeakButton);

    ditherBox.addItemList (ditherChoices, 1);
    ditherAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.apvts, id::dither, ditherBox);
    ditherBox.setTooltip (help::dither);
    ditherBox.setTitle ("Dither");
    content.addAndMakeVisible (ditherBox);

    shapingBox.addItemList (shapingChoices, 1);
    shapingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.apvts, id::noiseShaping, shapingBox);
    shapingBox.setTooltip (help::noiseShaping);
    shapingBox.setTitle ("Noise Shaping");
    content.addAndMakeVisible (shapingBox);

    // Meter strip + loudness suite ----------------------------------------
    for (auto* m : { (juce::Component*) &inMeter, (juce::Component*) &grMeter,
                     (juce::Component*) &outMeter })
        content.addAndMakeVisible (*m);

    auto initMeterLabel = [this] (juce::Label& l, const char* text,
                                  juce::Justification just = juce::Justification::centred)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        l.setColour (juce::Label::textColourId, colors::textDim);
        l.setJustificationType (just);
        content.addAndMakeVisible (l);
    };
    initMeterLabel (inLabel, "IN");
    initMeterLabel (grLabel, "GR");
    initMeterLabel (outLabel, "OUT");
    initMeterLabel (grValueLabel, "0.0 dB");
    grValueLabel.setColour (juce::Label::textColourId, colors::gainRed);

    initMeterLabel (mLabel, "M   --.-", juce::Justification::centredLeft);
    initMeterLabel (sLabel, "S   --.-", juce::Justification::centredLeft);
    initMeterLabel (iLabel, "I   --.-", juce::Justification::centredLeft);
    initMeterLabel (lraLabel, "LRA  -.-", juce::Justification::centredLeft);
    initMeterLabel (tpLabel, "TP  --.-", juce::Justification::centredLeft);
    iLabel.setColour (juce::Label::textColourId, colors::text);

    standardBox.addItemList ({ "EBU R128", "ATSC A/85" }, 1);
    standardBox.setTooltip (help::standardBox);
    standardBox.setTitle ("Loudness Standard");
    standardBox.onChange = [this]
    {
        settings->setValue ("loudnessStandard", standardBox.getSelectedId());
        settings->saveIfNeeded();
    };
    content.addAndMakeVisible (standardBox);
    standardBox.setSelectedId (settings->getIntValue ("loudnessStandard", 1),
                               juce::dontSendNotification);

    resetLoudnessButton.onClick = [this]
    {
        processor.loudness.resetIntegration();
        tpMaxLin = 0.0f;
    };
    resetLoudnessButton.setTooltip (help::resetLoudness);
    resetLoudnessButton.setTitle ("Reset Loudness");
    content.addAndMakeVisible (resetLoudnessButton);

    meterScaleBox.addItemList ({ "FULL", "-30 dB", "K-20", "K-14", "K-12" }, 1);
    meterScaleBox.setTooltip (help::meterScale);
    meterScaleBox.setTitle ("Meter Scale");
    meterScaleBox.onChange = [this]
    {
        applyMeterScale();
        settings->setValue ("meterScale", meterScaleBox.getSelectedId());
        settings->saveIfNeeded();
    };
    content.addAndMakeVisible (meterScaleBox);
    meterScaleBox.setSelectedId (settings->getIntValue ("meterScale", 1),
                                 juce::dontSendNotification);
    applyMeterScale();

    // Help hints bar + shortcuts overlay (v0.7) ----------------------------
    hintBar.setFont (juce::Font (juce::FontOptions (12.0f)));
    hintBar.setJustificationType (juce::Justification::centredLeft);
    if (helpHintsOn)
        hintBar.setText ("Hover any control for help. Press H to hide this bar, "
                         "F1 for shortcuts.", juce::dontSendNotification);
    content.addChildComponent (hintBar);
    hintBar.setVisible (helpHintsOn);

    content.addChildComponent (shortcutsOverlay);

    // Preset browser overlay ----------------------------------------------
    content.addChildComponent (presetBrowser);

    applyStaticColours();

    // Resizable shell: content at logical size, scaled by the editor.
    compact = settings->getBoolValue ("ui_compact", false);
    compactButton.setToggleState (compact, juce::dontSendNotification);

    setResizable (true, true);
    if (auto* c = getConstrainer())
    {
        c->setFixedAspectRatio ((double) logicalWidth() / (double) logicalHeight());
        c->setSizeLimits (logicalWidth() / 2, logicalHeight() / 2,
                          logicalWidth() * 2, logicalHeight() * 2);
    }
    const double scale = juce::jlimit (0.5, 2.0, settings->getDoubleValue ("ui_scale", 1.0));
    setSize (juce::roundToInt (logicalWidth() * scale),
             juce::roundToInt (logicalHeight() * scale));

    // GPU-accelerated compositing; repaints stay timer-driven.
    glContext.setContinuousRepainting (false);
    glContext.attachTo (*this);

    startTimerHz (30);
}

ZenithAudioProcessorEditor::~ZenithAudioProcessorEditor()
{
    glContext.detach();
    settings->saveIfNeeded();
    setLookAndFeel (nullptr);
}

void ZenithAudioProcessorEditor::initKnob (Knob& k, const juce::String& paramId,
                                           const juce::String& caption)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 66, 16);
    k.slider.setTitle (caption);              // screen readers
    k.slider.setWantsKeyboardFocus (true);    // tab traversal + arrow keys
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.apvts, paramId, k.slider);
    content.addAndMakeVisible (k.slider);
    midiLearnTargets[&k.slider] = paramId;

    k.caption.setText (caption, juce::dontSendNotification);
    k.caption.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    k.caption.setColour (juce::Label::textColourId, colors::textDim);
    k.caption.setJustificationType (juce::Justification::centred);
    content.addAndMakeVisible (k.caption);
}

void ZenithAudioProcessorEditor::showMidiLearnMenu (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;

    const auto it = midiLearnTargets.find (e.originalComponent);
    if (it == midiLearnTargets.end())
        return;

    const juce::String paramId = it->second;
    const int mappedCc = processor.getMappedCc (paramId);

    juce::PopupMenu menu;
    menu.addItem (1, "MIDI Learn");
    if (mappedCc >= 0)
        menu.addItem (2, "Remove MIDI CC " + juce::String (mappedCc));
    if (processor.isMidiLearnArmed())
        menu.addItem (3, "Cancel MIDI Learn");

    menu.showMenuAsync (
        juce::PopupMenu::Options().withTargetComponent (e.eventComponent),
        [this, paramId] (int result)
        {
            if (result == 1)
                processor.armMidiLearn (paramId);
            else if (result == 2)
                processor.clearMidiMapping (paramId);
            else if (result == 3)
                processor.cancelMidiLearn();
        });
}

void ZenithAudioProcessorEditor::layoutKnob (Knob& k, juce::Rectangle<int> area)
{
    k.caption.setBounds (area.removeFromTop (16));
    k.slider.setBounds (area);
}

void ZenithAudioProcessorEditor::paintContent (juce::Graphics& g)
{
    g.fillAll (colors::background);

    // Header bar.
    auto header = juce::Rectangle<int> (0, 0, logicalWidth(), 44);
    g.setColour (colors::panel);
    g.fillRect (header);
    g.setColour (colors::outline);
    g.fillRect (header.removeFromBottom (1));

    // Product mark: the family disc, with a dot at its zenith point.
    const float cx = 24.0f, cy = 22.0f, r = 9.0f;
    g.setColour (colors::accent);
    g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 1.6f);
    g.fillEllipse (cx - 2.4f, cy - r - 2.4f, 4.8f, 4.8f);

    g.setColour (colors::text);
    g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    g.drawText ("ZENITH", 42, 0, 110, 44, juce::Justification::centredLeft);

    // Meter strip panel (stops above the hint bar when it is shown).
    const int bodyH = logicalHeight() - (helpHintsOn ? hintBarH : 0);
    g.setColour (colors::panel);
    g.fillRoundedRectangle (juce::Rectangle<int> (logicalWidth() - 166, 52,
                                                  158, bodyH - 60)
                                .toFloat(), 5.0f);
}

void ZenithAudioProcessorEditor::resized()
{
    const float scale = (float) getWidth() / (float) logicalWidth();
    content.setTransform (juce::AffineTransform::scale (scale));
    content.setBounds (0, 0, logicalWidth(), logicalHeight());
    layoutContent();

    if (settings != nullptr)
        settings->setValue ("ui_scale", (double) scale);
}

void ZenithAudioProcessorEditor::layoutContent()
{
    presetBrowser.setBounds (content.getLocalBounds());
    shortcutsOverlay.setBounds (content.getLocalBounds());

    auto area = content.getLocalBounds();

    // Help hints strip along the bottom (when enabled).
    hintBar.setBounds (helpHintsOn ? area.removeFromBottom (hintBarH).reduced (8, 0)
                                   : juce::Rectangle<int>());

    // Header.
    auto header = area.removeFromTop (44).reduced (8, 9);
    header.removeFromLeft (140); // mark + title, painted

    undoButton.setBounds (header.removeFromLeft (52));
    header.removeFromLeft (4);
    redoButton.setBounds (header.removeFromLeft (52));
    header.removeFromLeft (14);
    slotAButton.setBounds (header.removeFromLeft (28));
    header.removeFromLeft (4);
    slotBButton.setBounds (header.removeFromLeft (28));
    header.removeFromLeft (4);
    copySlotButton.setBounds (header.removeFromLeft (52));

    bypassButton.setBounds (header.removeFromRight (70));
    header.removeFromRight (8);
    compactButton.setBounds (header.removeFromRight (74));
    header.removeFromRight (8);
    helpButton.setBounds (header.removeFromRight (46));
    header.removeFromRight (8);
    nextPresetButton.setBounds (header.removeFromRight (24));
    presetNameButton.setBounds (header.removeFromRight (156));
    prevPresetButton.setBounds (header.removeFromRight (24));
    header.removeFromRight (8);
    cpuLabel.setBounds (header.removeFromRight (58));

    // Meter strip + loudness suite. bodyH matches the panel drawn in
    // paintContent(): the window height minus the hint bar.
    const int bodyH = logicalHeight() - (helpHintsOn ? hintBarH : 0);
    auto meters = juce::Rectangle<int> (logicalWidth() - 166, 52, 158,
                                        bodyH - 60)
                      .reduced (10, 8);
    meterScaleBox.setBounds (meters.removeFromBottom (22));
    meters.removeFromBottom (4);
    {
        auto row = meters.removeFromBottom (20);
        standardBox.setBounds (row.removeFromLeft (84));
        row.removeFromLeft (4);
        resetLoudnessButton.setBounds (row);
    }
    meters.removeFromBottom (4);
    tpLabel.setBounds (meters.removeFromBottom (15));
    lraLabel.setBounds (meters.removeFromBottom (15));
    iLabel.setBounds (meters.removeFromBottom (15));
    sLabel.setBounds (meters.removeFromBottom (15));
    mLabel.setBounds (meters.removeFromBottom (15));
    meters.removeFromBottom (4);
    grValueLabel.setBounds (meters.removeFromBottom (16));

    auto labels = meters.removeFromBottom (16);
    const int meterW = meters.getWidth() / 3;
    inLabel.setBounds (labels.removeFromLeft (meterW));
    grLabel.setBounds (labels.removeFromLeft (meterW));
    outLabel.setBounds (labels);
    inMeter.setBounds (meters.removeFromLeft (meterW).reduced (6, 0));
    grMeter.setBounds (meters.removeFromLeft (meterW).reduced (6, 0));
    outMeter.setBounds (meters.reduced (6, 0));

    // Left column.
    auto contentArea = area.withTrimmedRight (166).reduced (16, 8);

    display.setVisible (! compact);
    if (! compact)
    {
        display.setBounds (contentArea.removeFromTop (176));
        contentArea.removeFromTop (8);
    }

    auto bigRow = contentArea.removeFromTop (150);
    const int bigW = bigRow.getWidth() / 3;
    layoutKnob (gainKnob, bigRow.removeFromLeft (bigW).reduced (10, 0));
    layoutKnob (ceilingKnob, bigRow.removeFromLeft (bigW).reduced (10, 0));
    layoutKnob (releaseKnob, bigRow.reduced (10, 0));

    contentArea.removeFromTop (8);

    auto smallRow = contentArea.removeFromTop (96);
    const int smallW = smallRow.getWidth() / 4;
    layoutKnob (attackKnob, smallRow.removeFromLeft (smallW).reduced (8, 0));
    layoutKnob (lookaheadKnob, smallRow.removeFromLeft (smallW).reduced (8, 0));
    layoutKnob (linkTransientKnob, smallRow.removeFromLeft (smallW).reduced (8, 0));
    layoutKnob (linkReleaseKnob, smallRow.reduced (8, 0));

    contentArea.removeFromTop (8);

    auto styleRow = contentArea.removeFromTop (26);
    styleCaption.setBounds (styleRow.removeFromLeft (44));
    styleBox.setBounds (styleRow.removeFromLeft (130));
    styleRow.removeFromLeft (18);
    autoReleaseButton.setBounds (styleRow.removeFromLeft (76));
    styleRow.removeFromLeft (6);
    dcFilterButton.setBounds (styleRow.removeFromLeft (46));
    styleRow.removeFromLeft (6);
    unityGainButton.setBounds (styleRow.removeFromLeft (60));
    styleRow.removeFromLeft (6);
    auditionButton.setBounds (styleRow.removeFromLeft (60));
    styleRow.removeFromLeft (6);
    extScButton.setBounds (styleRow.removeFromLeft (62));

    contentArea.removeFromTop (8);

    auto outputRow = contentArea.removeFromTop (26);
    outputCaption.setBounds (outputRow.removeFromLeft (52));
    osBox.setBounds (outputRow.removeFromLeft (76));
    outputRow.removeFromLeft (8);
    truePeakButton.setBounds (outputRow.removeFromLeft (84));
    outputRow.removeFromLeft (16);
    ditherBox.setBounds (outputRow.removeFromLeft (76));
    outputRow.removeFromLeft (6);
    shapingBox.setBounds (outputRow.removeFromLeft (100));
}

// Re-derives the constrainer and window size after anything changes the
// logical size (Compact toggle, hint bar), preserving the user's scale.
void ZenithAudioProcessorEditor::applyLogicalSize()
{
    const float scale = (float) getWidth() / (float) logicalWidth();

    if (auto* c = getConstrainer())
    {
        c->setFixedAspectRatio ((double) logicalWidth() / (double) logicalHeight());
        c->setSizeLimits (logicalWidth() / 2, logicalHeight() / 2,
                          logicalWidth() * 2, logicalHeight() * 2);
    }
    setSize (juce::roundToInt (logicalWidth() * scale),
             juce::roundToInt (logicalHeight() * scale));
}

void ZenithAudioProcessorEditor::setCompact (bool shouldBeCompact, bool save)
{
    if (compact == shouldBeCompact)
        return;

    compact = shouldBeCompact;
    compactButton.setToggleState (compact, juce::dontSendNotification);
    applyLogicalSize();

    if (save)
    {
        settings->setValue ("ui_compact", compact);
        settings->saveIfNeeded();
    }
}

// Help & accessibility (v0.7) ------------------------------------------------

void ZenithAudioProcessorEditor::showHelpMenu()
{
    const float scale = lookAndFeel.getTextScale();

    juce::PopupMenu sizes;
    sizes.addItem (10, "100%", true, juce::approximatelyEqual (scale, 1.0f));
    sizes.addItem (11, "115%", true, juce::approximatelyEqual (scale, 1.15f));
    sizes.addItem (12, "130%", true, juce::approximatelyEqual (scale, 1.3f));

    juce::PopupMenu menu;
    menu.addItem (1, "Open Manual...");
    menu.addItem (2, "Keyboard Shortcuts (F1)");
    menu.addSeparator();
    menu.addItem (3, "Help Hints Bar (H)", true, helpHintsOn);
    menu.addItem (4, "High Contrast", true, jdm::colors::highContrast);
    menu.addSubMenu ("Text Size", sizes);

    menu.showMenuAsync (
        juce::PopupMenu::Options().withTargetComponent (&helpButton),
        [this] (int result)
        {
            switch (result)
            {
                case 1: openManual(); break;
                case 2:
                    shortcutsOverlay.setVisible (true);
                    shortcutsOverlay.toFront (true);
                    break;
                case 3: setHelpHints (! helpHintsOn, true); break;
                case 4: setHighContrast (! jdm::colors::highContrast); break;
                case 10: setTextScale (1.0f); break;
                case 11: setTextScale (1.15f); break;
                case 12: setTextScale (1.3f); break;
                default: break;
            }
        });
}

void ZenithAudioProcessorEditor::openManual()
{
    // The manual ships inside the binary (no installer yet); materialize it
    // next to the settings file and open the default browser.
    auto file = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("Equinox Audio").getChildFile ("Zenith Manual.html");
    file.getParentDirectory().createDirectory();
    if (file.replaceWithText (znth::help::buildManual()))
        juce::URL (file).launchInDefaultBrowser();
}

void ZenithAudioProcessorEditor::setHelpHints (bool on, bool save)
{
    if (helpHintsOn == on)
        return;

    helpHintsOn = on;
    hintBar.setVisible (on);
    hintBar.setText (on ? juce::String ("Hover any control for help. "
                                        "Press H to hide this bar, F1 for shortcuts.")
                        : juce::String(),
                     juce::dontSendNotification);
    applyLogicalSize();

    if (save)
    {
        settings->setValue ("help_hints", on);
        settings->saveIfNeeded();
    }
}

void ZenithAudioProcessorEditor::setHighContrast (bool on)
{
    jdm::colors::setHighContrast (on);
    lookAndFeel.applyPalette();
    settings->setValue ("high_contrast", on);
    settings->saveIfNeeded();
    applyStaticColours();

    sendLookAndFeelChange();
    repaint();
}

void ZenithAudioProcessorEditor::setTextScale (float scale)
{
    lookAndFeel.setTextScale (scale);
    settings->setValue ("text_scale", scale);
    settings->saveIfNeeded();

    sendLookAndFeelChange();
    repaint();
}

// Labels cache colour values at set time; anything set explicitly from the
// palette globals must be re-applied when the palette swaps.
void ZenithAudioProcessorEditor::applyStaticColours()
{
    cpuLabel.setColour (juce::Label::textColourId, colors::textDim);
    styleCaption.setColour (juce::Label::textColourId, colors::textDim);
    outputCaption.setColour (juce::Label::textColourId, colors::textDim);

    for (auto* k : { &gainKnob, &ceilingKnob, &releaseKnob, &attackKnob, &lookaheadKnob,
                     &linkTransientKnob, &linkReleaseKnob })
        k->caption.setColour (juce::Label::textColourId, colors::textDim);

    for (auto* l : { &inLabel, &grLabel, &outLabel, &mLabel, &sLabel, &lraLabel, &tpLabel })
        l->setColour (juce::Label::textColourId, colors::textDim);
    grValueLabel.setColour (juce::Label::textColourId, colors::gainRed);
    iLabel.setColour (juce::Label::textColourId, colors::text);

    hintBar.setColour (juce::Label::textColourId, colors::text);
    hintBar.setColour (juce::Label::backgroundColourId, colors::panel.withAlpha (0.6f));
}

// Walk up from the hovered component to find something with help text; the
// display and meters aren't tooltip clients, so they get named directly.
void ZenithAudioProcessorEditor::updateHintBar (const juce::MouseEvent& e)
{
    if (! helpHintsOn)
        return;

    juce::String text;
    for (auto* c = e.eventComponent; c != nullptr && c != &content; c = c->getParentComponent())
    {
        if (c == &display)
        {
            text = help::mainDisplayText;
            break;
        }
        if (c == &inMeter || c == &grMeter || c == &outMeter)
        {
            text = help::meterStrip;
            break;
        }
        if (auto* client = dynamic_cast<juce::TooltipClient*> (c))
        {
            if (auto tip = client->getTooltip(); tip.isNotEmpty())
            {
                text = tip;
                break;
            }
        }
    }

    hintBar.setText (text, juce::dontSendNotification);
}

void ZenithAudioProcessorEditor::ShortcutsOverlay::paint (juce::Graphics& g)
{
    g.fillAll (colors::background.withAlpha (0.82f));

    constexpr int rowH = 22;
    const int numRows = (int) std::size (help::shortcuts);
    auto panel = getLocalBounds().withSizeKeepingCentre (
        440, numRows * rowH + 58).toFloat();

    g.setColour (colors::panel);
    g.fillRoundedRectangle (panel, 6.0f);
    g.setColour (colors::outline);
    g.drawRoundedRectangle (panel.reduced (0.5f), 6.0f, 1.0f);

    auto inner = panel.reduced (16.0f, 12.0f);
    g.setColour (colors::text);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawText ("KEYBOARD SHORTCUTS", inner.removeFromTop (26.0f),
                juce::Justification::centredLeft);

    for (const auto& s : help::shortcuts)
    {
        auto row = inner.removeFromTop ((float) rowH);
        g.setColour (colors::accent);
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText (s.keys, row.removeFromLeft (170.0f), juce::Justification::centredLeft);
        g.setColour (colors::text);
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawText (s.action, row, juce::Justification::centredLeft);
    }

    g.setColour (colors::textDim);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText ("click anywhere or press any key to close",
                panel.reduced (16.0f, 6.0f).removeFromBottom (14.0f),
                juce::Justification::centredRight);
}

void ZenithAudioProcessorEditor::applyMeterScale()
{
    float minDb = -60.0f, refDb = znth::LevelMeter::noRef;
    switch (meterScaleBox.getSelectedId())
    {
        case 2: minDb = -30.0f; break;
        case 3: minDb = -44.0f; refDb = -20.0f; break; // K-20
        case 4: minDb = -38.0f; refDb = -14.0f; break; // K-14
        case 5: minDb = -36.0f; refDb = -12.0f; break; // K-12
        default: break;
    }
    inMeter.setScale (minDb, refDb);
    outMeter.setScale (minDb, refDb);
}

bool ZenithAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    // Letter key codes arrive in platform-dependent case; normalize.
    int code = key.getKeyCode();
    if (code < 256)
        code = juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) code);

    if (mods.isCommandDown())
    {
        if (code == 'Z' && mods.isShiftDown()) { processor.stateManager.redo(); return true; }
        if (code == 'Z')                       { processor.stateManager.undo(); return true; }
        if (code == 'Y')                       { processor.stateManager.redo(); return true; }
        return false;
    }

    if (key == juce::KeyPress::F1Key)
    {
        shortcutsOverlay.setVisible (! shortcutsOverlay.isVisible());
        shortcutsOverlay.toFront (true);
        return true;
    }

    if (code == juce::KeyPress::escapeKey)
    {
        if (shortcutsOverlay.isVisible()) { shortcutsOverlay.setVisible (false); return true; }
        if (presetBrowser.isVisible())    { presetBrowser.closeBrowser(); return true; }
        return false;
    }

    if (mods.isShiftDown())
    {
        if (code == 'B')
        {
            if (auto* param = processor.apvts.getParameter (znth::id::bypass))
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (param->getValue() > 0.5f ? 0.0f : 1.0f);
                param->endChangeGesture();
            }
            return true;
        }
        return false;
    }

    if (mods.isAnyModifierKeyDown())
        return false;

    switch (code)
    {
        case 'A': processor.stateManager.setActiveSlot (0); return true;
        case 'B': processor.stateManager.setActiveSlot (1); return true;
        case 'C': processor.stateManager.copyActiveToOther(); return true;
        case 'P':
            if (presetBrowser.isVisible()) presetBrowser.closeBrowser();
            else                           presetBrowser.openBrowser();
            return true;
        case '[': processor.presets.loadAdjacent (false); return true;
        case ']': processor.presets.loadAdjacent (true); return true;
        case 'H': setHelpHints (! helpHintsOn, true); return true;
        case 'K': setCompact (! compact, true); return true;
        default: break;
    }
    return false;
}

void ZenithAudioProcessorEditor::timerCallback()
{
    // Persist any MIDI mapping the audio thread captured since last tick.
    processor.commitPendingMidiLearn();

    inMeter.push (processor.peakIn.exchange (0.0f));
    outMeter.push (processor.peakOut.exchange (0.0f));
    grMeter.push (processor.gainReductionDb.load());

    grValueLabel.setText (juce::String (grMeter.current(), 1) + " dB",
                          juce::dontSendNotification);

    // Loudness suite (message-thread analysis) + max true peak.
    processor.loudness.update();
    auto fmt = [] (const char* tag, float v, const char* unit)
    {
        return juce::String (tag)
               + (v <= jdm::LoudnessSuite::silence + 1.0f ? juce::String ("  --.-")
                                                          : juce::String (v, 1))
               + " " + unit;
    };
    const bool atsc = standardBox.getSelectedId() == 2;
    const char* lu = atsc ? "LKFS" : "LUFS";
    mLabel.setText (fmt ("M  ", processor.loudness.momentaryLufs(), lu),
                    juce::dontSendNotification);
    sLabel.setText (fmt ("S  ", processor.loudness.shortTermLufs(), lu),
                    juce::dontSendNotification);

    const float integrated = processor.loudness.integratedLufs();
    const float target = atsc ? -24.0f : -23.0f;
    iLabel.setText (fmt ("I  ", integrated, lu), juce::dontSendNotification);
    iLabel.setColour (juce::Label::textColourId,
                      integrated > target + 0.5f ? colors::warn : colors::text);

    lraLabel.setText ("LRA " + juce::String (processor.loudness.loudnessRangeLu(), 1) + " LU",
                      juce::dontSendNotification);

    tpMaxLin = juce::jmax (tpMaxLin, processor.outTruePeak.readAndReset());
    const float tpDb = juce::Decibels::gainToDecibels (tpMaxLin, -100.0f);
    tpLabel.setText (tpDb <= -99.0f ? juce::String ("TP  --.- dBTP")
                                    : "TP " + juce::String (tpDb, 1) + " dBTP",
                     juce::dontSendNotification);
    tpLabel.setColour (juce::Label::textColourId,
                       tpDb > 0.0f ? colors::warn : colors::textDim);

    undoButton.setEnabled (processor.stateManager.canUndo());
    redoButton.setEnabled (processor.stateManager.canRedo());

    const int active = processor.stateManager.getActiveSlot();
    slotAButton.setToggleState (active == 0, juce::dontSendNotification);
    slotBButton.setToggleState (active == 1, juce::dontSendNotification);

    const auto preset = processor.stateManager.getCurrentPresetName();
    presetNameButton.setButtonText (preset.isEmpty() ? "PRESETS" : preset);

    cpuLabel.setText ("CPU " + juce::String (processor.cpuLoad.load() * 100.0f, 1) + "%",
                      juce::dontSendNotification);
}
