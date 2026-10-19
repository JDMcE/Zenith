#include "PluginProcessor.h"
#include "PluginEditor.h"

ZenithAudioProcessor::ZenithAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "ZenithState", znth::createParameterLayout())
{
    // Undo/redo, A/B slots, and preset loads all reapply whole state trees;
    // the CC lookup is a cache built from the tree, so refresh it each time.
    stateManager.onStateReapplied = [this] { rebuildMidiCcMap(); };

    gainParam          = apvts.getRawParameterValue (znth::id::gain);
    ceilingParam       = apvts.getRawParameterValue (znth::id::ceiling);
    releaseParam       = apvts.getRawParameterValue (znth::id::release);
    autoReleaseParam   = apvts.getRawParameterValue (znth::id::autoRelease);
    attackParam        = apvts.getRawParameterValue (znth::id::attack);
    lookaheadParam     = apvts.getRawParameterValue (znth::id::lookahead);
    styleParam         = apvts.getRawParameterValue (znth::id::style);
    linkTransientParam = apvts.getRawParameterValue (znth::id::linkTransient);
    linkReleaseParam   = apvts.getRawParameterValue (znth::id::linkRelease);
    dcFilterParam      = apvts.getRawParameterValue (znth::id::dcFilter);
    oversamplingParam  = apvts.getRawParameterValue (znth::id::oversampling);
    truePeakParam      = apvts.getRawParameterValue (znth::id::truePeak);
    ditherParam        = apvts.getRawParameterValue (znth::id::dither);
    noiseShapingParam  = apvts.getRawParameterValue (znth::id::noiseShaping);
    unityGainParam     = apvts.getRawParameterValue (znth::id::unityGain);
    auditionParam      = apvts.getRawParameterValue (znth::id::audition);
    extSidechainParam  = apvts.getRawParameterValue (znth::id::extSidechain);
    bypassParam        = apvts.getRawParameterValue (znth::id::bypass);

    rebuildMidiCcMap();
}

bool ZenithAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();

    // Main path: any named layout up to 8 channels (mono, stereo, LCR,
    // quad, 5.0/5.1 ... 7.1), in == out. Surround link topology lives in
    // the engine (LFE decoupled, full-range channels grouped).
    if (in != out || in.isDisabled() || in.isDiscreteLayout()
        || in.size() < 1 || in.size() > znth::LimiterEngine::maxChannels)
        return false;

    // Side chain (bus 1): disabled, mono, stereo, or mirroring the main
    // layout (a narrower side chain is mapped across the mains).
    if (layouts.inputBuses.size() > 1)
    {
        const auto& sc = layouts.getChannelSet (true, 1);
        if (! (sc.isDisabled()
               || sc == juce::AudioChannelSet::mono()
               || sc == juce::AudioChannelSet::stereo()
               || sc == in))
            return false;
    }

    return true;
}

void ZenithAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate = sampleRate;
    const int numChannels = juce::jlimit (1, maxChans, getMainBusNumInputChannels());
    const int scChannels = getChannelCountOfBus (true, 1);

    for (size_t i = 1; i < oversamplers.size(); ++i)
    {
        // useIntegerLatency stays OFF: JUCE's integer-latency mode inserts a
        // fractional-delay interpolator after the downsampler, and that
        // interpolator overshoots on transients — it can push a limited
        // click ~1 dB back over the ceiling (observed at 4x, whose FIR
        // latency is fractional). We report the rounded latency instead;
        // half a sample of PDC skew is harmless, a breached ceiling is not.
        oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
            (size_t) numChannels, i,
            juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        oversamplers[i]->initProcessing ((size_t) samplesPerBlock);

        // Same design for the side chain: identical up-path latency keeps
        // the detector aligned with the mains inside the engine.
        if (scChannels > 0)
        {
            scOversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
                (size_t) scChannels, i,
                juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
            scOversamplers[i]->initProcessing ((size_t) samplesPerBlock);
        }
        else
            scOversamplers[i].reset();
    }

    loudness.prepare (sampleRate, numChannels);

    // BS.1770-4 surround weighting: surrounds count 1.41 (~+1.5 dB), the
    // LFE is excluded from the loudness measurement entirely.
    {
        const auto layout = getChannelLayoutOfBus (false, 0);
        std::array<float, maxChans> w {};
        for (int c = 0; c < numChannels; ++c)
        {
            using CS = juce::AudioChannelSet;
            switch (layout.getTypeOfChannel (c))
            {
                case CS::LFE:
                case CS::LFE2:                w[(size_t) c] = 0.0f;  break;
                case CS::leftSurround:
                case CS::rightSurround:
                case CS::leftSurroundSide:
                case CS::rightSurroundSide:
                case CS::leftSurroundRear:
                case CS::rightSurroundRear:
                case CS::surround:            w[(size_t) c] = 1.41f; break;
                default:                      w[(size_t) c] = 1.0f;  break;
            }
        }
        loudness.setChannelWeights (w.data(), numChannels);
    }

    dither.prepare (numChannels);
    outTruePeak.prepare (numChannels);

    gainSmooth.reset (sampleRate, 0.02);
    outScaleSmooth.reset (sampleRate, 0.02);
    bypassMix.reset (sampleRate, 0.01);

    gainSmooth.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParam->load()));
    outScaleSmooth.setCurrentAndTargetValue (1.0f);
    bypassMix.setCurrentAndTargetValue (bypassParam->load() > 0.5f ? 1.0f : 0.0f);

    dcCoeff = 1.0f - (float) std::exp (-juce::MathConstants<double>::twoPi * 5.0 / sampleRate);
    dcState.fill (0.0f);

    dryBuffer.setSize (numChannels, samplesPerBlock);
    delayedDryBuffer.setSize (numChannels, samplesPerBlock * 32); // OS-rate scratch too
    scopeInBuffer.setSize (1, samplesPerBlock);
    scopeAccIn = scopeAccOut = scopeAccGr = 0.0f;
    scopeCellPos = 0;

    lastReportedLatency = -1;
    currentOsApplied = -1; // force engine re-prepare at the effective rate
    applyOversamplingChoice();
}

// True Peak mode enforces at least 4x: the limiter has to run at a rate
// where inter-sample peaks exist. The engine is re-prepared at the new
// effective rate (envelope reset — a factor change is a structural edit).
void ZenithAudioProcessor::applyOversamplingChoice()
{
    const int choice = juce::jlimit (0, (int) oversamplers.size() - 1,
                                     (int) oversamplingParam->load());
    const int effective = truePeakParam->load() > 0.5f
        ? juce::jmax (choice, znth::minTruePeakOsChoice)
        : choice;

    if (effective == currentOsApplied)
        return;

    currentOsApplied = effective;
    const int factor = znth::oversamplingFactors[effective];
    engine.prepare (hostSampleRate * factor,
                    juce::jlimit (1, maxChans, getMainBusNumInputChannels()),
                    getChannelLayoutOfBus (false, 0)
                        .getChannelIndexForType (juce::AudioChannelSet::LFE));
    if (effective > 0)
    {
        oversamplers[(size_t) effective]->reset();
        if (auto& sc = scOversamplers[(size_t) effective])
            sc->reset();
    }
    updateLatency();
}

void ZenithAudioProcessor::updateLatency()
{
    const int factor = znth::oversamplingFactors[juce::jmax (0, currentOsApplied)];
    const int osLatency = currentOsApplied > 0
        ? juce::roundToInt (oversamplers[(size_t) currentOsApplied]->getLatencyInSamples())
        : 0;

    // Engine lookahead runs at the oversampled rate; convert to host samples.
    const int lookahead = juce::roundToInt (
        (double) engine.getLookaheadSamples() / (double) factor);

    const int total = osLatency + lookahead;
    if (total != lastReportedLatency)
    {
        lastReportedLatency = total;
        setLatencySamples (total);
    }
}

void ZenithAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    handleMidi (midi); // MIDI Learn capture + mapped CC -> parameter

    const auto blockStartTicks = juce::Time::getHighResolutionTicks();

    auto mainBuf = getBusBuffer (buffer, false, 0);

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (mainBuf.getNumChannels(), maxChans);

    if (numSamples == 0 || numChannels == 0)
        return;

    applyOversamplingChoice(); // before setParams: engine rate must be current

    znth::LimiterEngine::Params p;
    p.ceilingDb       = ceilingParam->load();
    p.releaseMs       = releaseParam->load();
    p.autoRelease     = autoReleaseParam->load() > 0.5f;
    p.attackMs        = attackParam->load();
    p.lookaheadMs     = lookaheadParam->load();
    p.style           = (znth::Style) (int) styleParam->load();
    p.linkTransient01 = linkTransientParam->load() * 0.01f;
    p.linkRelease01   = linkReleaseParam->load() * 0.01f;
    engine.setParams (p);
    updateLatency();

    float* chans[maxChans] = {};
    for (int ch = 0; ch < numChannels; ++ch)
        chans[ch] = mainBuf.getWritePointer (ch);

    // External side chain (v0.5): when engaged and the host feeds the bus,
    // the detector reads it instead of the main input. The signal is taken
    // as-is — deliberately outside the input gain — so every stem instance
    // sharing a trigger computes the identical gain envelope regardless of
    // its own drive.
    const float* scChans[maxChans] = {};
    int scCount = 0;
    if (extSidechainParam->load() > 0.5f && getBusCount (true) > 1)
    {
        auto scBuf = getBusBuffer (buffer, true, 1);
        scCount = juce::jmin (scBuf.getNumChannels(), maxChans);
        for (int ch = 0; ch < scCount; ++ch)
            scChans[ch] = scBuf.getReadPointer (ch);
    }
    const bool extScOn = scCount > 0;

    // Keep the raw input for the click-free bypass crossfade.
    dryBuffer.setSize (numChannels, numSamples, false, false, true);
    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom (ch, 0, mainBuf, ch, 0, numSamples);

    // DC filter (pre-gain) + input gain, main bus only. peakIn is the level
    // hitting the limiter, i.e. post-gain.
    const bool dcOn = dcFilterParam->load() > 0.5f;
    gainSmooth.setTargetValue (juce::Decibels::decibelsToGain (gainParam->load()));

    float inPk = 0.0f;
    float* scopeIn = scopeInBuffer.getWritePointer (0);
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = gainSmooth.getNextValue();
        float samplePk = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float x = chans[ch][i];
            if (dcOn)
            {
                dcState[(size_t) ch] += dcCoeff * (x - dcState[(size_t) ch]);
                x -= dcState[(size_t) ch];
            }
            x *= g;
            chans[ch][i] = x;
            samplePk = juce::jmax (samplePk, std::abs (x));
        }
        scopeIn[i] = samplePk; // what hits the limiter, for the display
        inPk = juce::jmax (inPk, samplePk);
    }

    // Limit — at the oversampled rate when a factor is active. The engine
    // also hands back the lookahead-delayed input (post gain), the
    // reference for the delta audition path; with oversampling the delta is
    // formed at the oversampled rate so one downsample serves both modes.
    // The side chain runs through its own identical up-path so detector and
    // mains stay sample-aligned (it is never downsampled — nobody hears it).
    const bool auditionOn = auditionParam->load() > 0.5f;
    float blockGrDb = 0.0f;

    float* dryPtrs[maxChans] = {};
    if (auditionOn)
        for (int ch = 0; ch < numChannels; ++ch)
            dryPtrs[ch] = delayedDryBuffer.getWritePointer (ch);

    // Detector pointers mapped onto the main layout: a narrower side chain
    // broadcasts its last channel across the remaining mains.
    const float* detPtrs[maxChans] = {};

    if (currentOsApplied > 0)
    {
        auto& os = *oversamplers[(size_t) currentOsApplied];
        juce::dsp::AudioBlock<float> block (chans, (size_t) numChannels,
                                            (size_t) numSamples);
        auto osBlock = os.processSamplesUp (block);
        const int osSamples = (int) osBlock.getNumSamples();

        float* osPtrs[maxChans] = {};
        for (int ch = 0; ch < numChannels; ++ch)
            osPtrs[ch] = osBlock.getChannelPointer ((size_t) ch);

        const float* const* det = nullptr;
        if (extScOn && scOversamplers[(size_t) currentOsApplied] != nullptr)
        {
            juce::dsp::AudioBlock<const float> scBlock (scChans, (size_t) scCount,
                                                        (size_t) numSamples);
            auto scOsBlock = scOversamplers[(size_t) currentOsApplied]
                                 ->processSamplesUp (scBlock);
            for (int ch = 0; ch < numChannels; ++ch)
                detPtrs[ch] = scOsBlock.getChannelPointer (
                    (size_t) juce::jmin (ch, scCount - 1));
            det = detPtrs;
        }

        blockGrDb = engine.process (osPtrs, numChannels, osSamples, det,
                                    auditionOn ? dryPtrs : nullptr);

        if (auditionOn)
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < osSamples; ++i)
                    osPtrs[ch][i] = dryPtrs[ch][i] - osPtrs[ch][i];

        os.processSamplesDown (block);
    }
    else
    {
        const float* const* det = nullptr;
        if (extScOn)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                detPtrs[ch] = scChans[juce::jmin (ch, scCount - 1)];
            det = detPtrs;
        }

        blockGrDb = engine.process (chans, numChannels, numSamples, det,
                                    auditionOn ? dryPtrs : nullptr);

        // Audition Limiting: output what the limiter removed instead of
        // what it kept. Below the ceiling this nulls to silence.
        if (auditionOn)
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < numSamples; ++i)
                    chans[ch][i] = dryPtrs[ch][i] - chans[ch][i];
    }

    // Unity Gain undoes the input gain at the output so the limiting is
    // judged at the incoming level.
    outScaleSmooth.setTargetValue (unityGainParam->load() > 0.5f
        ? 1.0f / juce::jmax (1.0e-3f, juce::Decibels::decibelsToGain (gainParam->load()))
        : 1.0f);

    // Output scale + bypass crossfade back to the raw dry signal.
    bypassMix.setTargetValue (bypassParam->load() > 0.5f ? 1.0f : 0.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        const float scale = outScaleSmooth.getNextValue();
        const float dry = bypassMix.getNextValue();
        const float wet = 1.0f - dry;

        for (int ch = 0; ch < numChannels; ++ch)
            chans[ch][i] = chans[ch][i] * scale * wet + dryBuffer.getSample (ch, i) * dry;
    }

    // Dither is the very last processing step, so the meters (and the
    // loudness/true-peak measurements) see exactly what leaves the plugin.
    dither.setParams (znth::ditherBits[juce::jlimit (0, 2, (int) ditherParam->load())],
                      (znth::NoiseShaping) (int) noiseShapingParam->load());
    dither.process (chans, numSamples);

    float outPk = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float samplePk = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            samplePk = juce::jmax (samplePk, std::abs (chans[ch][i]));
        outPk = juce::jmax (outPk, samplePk);

        // Scope cell accumulation for the main display.
        scopeAccIn = juce::jmax (scopeAccIn, scopeIn[i]);
        scopeAccOut = juce::jmax (scopeAccOut, samplePk);
        scopeAccGr = juce::jmax (scopeAccGr, blockGrDb);
        if (++scopeCellPos >= scopeCellSamples)
        {
            const auto idx = scopeWriteIndex.load (std::memory_order_relaxed);
            scopeRing[idx & (uint32_t) (scopeSize - 1)] = { scopeAccIn, scopeAccOut, scopeAccGr };
            scopeWriteIndex.store (idx + 1, std::memory_order_release);
            scopeAccIn = scopeAccOut = scopeAccGr = 0.0f;
            scopeCellPos = 0;
        }
    }

    loudness.process (chans, numSamples);
    outTruePeak.process (chans, numSamples);
    gainReductionDb.store (blockGrDb);

    // Peak-hold for the GUI (released when the GUI reads with exchange (0)).
    float prev = peakIn.load();
    while (inPk > prev && ! peakIn.compare_exchange_weak (prev, inPk)) {}
    prev = peakOut.load();
    while (outPk > prev && ! peakOut.compare_exchange_weak (prev, outPk)) {}

    // Smoothed fraction-of-realtime CPU estimate for the GUI.
    const double elapsed = juce::Time::highResolutionTicksToSeconds (
        juce::Time::getHighResolutionTicks() - blockStartTicks);
    const double budget = (double) numSamples / hostSampleRate;
    const float load = (float) (elapsed / juce::jmax (budget, 1.0e-9));
    cpuLoad.store (0.9f * cpuLoad.load() + 0.1f * load);
}

// MIDI Learn -----------------------------------------------------------------

static const juce::Identifier midiMapType ("MIDIMAP");
static const juce::Identifier midiMapEntryType ("MAP");
static const juce::Identifier midiMapCcProp ("cc");
static const juce::Identifier midiMapParamProp ("param");

void ZenithAudioProcessor::handleMidi (juce::MidiBuffer& midi)
{
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isController())
            continue;

        const int cc = msg.getControllerNumber();
        if (cc < 0 || cc >= (int) ccTargets.size())
            continue;

        // Armed learn: capture this CC. The lookup entry is live at once;
        // commitPendingMidiLearn() persists it on the message thread.
        if (auto* target = learnTarget.exchange (nullptr))
        {
            ccTargets[(size_t) cc].store (target);
            pendingLearnParam.store (target, std::memory_order_relaxed);
            pendingLearnCc.store (cc, std::memory_order_release);
        }

        if (auto* mapped = ccTargets[(size_t) cc].load())
            mapped->setValueNotifyingHost ((float) msg.getControllerValue() / 127.0f);
    }
}

void ZenithAudioProcessor::armMidiLearn (const juce::String& paramId)
{
    learnTarget.store (apvts.getParameter (paramId));
}

int ZenithAudioProcessor::getMappedCc (const juce::String& paramId) const
{
    auto map = apvts.state.getChildWithName (midiMapType);
    for (int i = 0; i < map.getNumChildren(); ++i)
        if (map.getChild (i)[midiMapParamProp].toString() == paramId)
            return map.getChild (i)[midiMapCcProp];
    return -1;
}

void ZenithAudioProcessor::clearMidiMapping (const juce::String& paramId)
{
    auto map = apvts.state.getChildWithName (midiMapType);
    for (int i = map.getNumChildren(); --i >= 0;)
        if (map.getChild (i)[midiMapParamProp].toString() == paramId)
            map.removeChild (i, nullptr);
    rebuildMidiCcMap();
    stateManager.noteNonParameterChange();
}

void ZenithAudioProcessor::commitPendingMidiLearn()
{
    const int cc = pendingLearnCc.exchange (-1, std::memory_order_acquire);
    if (cc < 0)
        return;

    auto* param = pendingLearnParam.load (std::memory_order_relaxed);
    if (param == nullptr)
        return;

    auto map = apvts.state.getOrCreateChildWithName (midiMapType, nullptr);

    // One CC per parameter and one parameter per CC: drop colliding entries.
    for (int i = map.getNumChildren(); --i >= 0;)
    {
        auto entry = map.getChild (i);
        if ((int) entry[midiMapCcProp] == cc
            || entry[midiMapParamProp].toString() == param->paramID)
            map.removeChild (i, nullptr);
    }

    juce::ValueTree entry (midiMapEntryType);
    entry.setProperty (midiMapCcProp, cc, nullptr);
    entry.setProperty (midiMapParamProp, param->paramID, nullptr);
    map.appendChild (entry, nullptr);

    rebuildMidiCcMap();

    // The mapping changed the state tree without touching a parameter —
    // tell the history so undo and session snapshots see it.
    stateManager.noteNonParameterChange();
}

void ZenithAudioProcessor::rebuildMidiCcMap()
{
    for (auto& t : ccTargets)
        t.store (nullptr);

    auto map = apvts.state.getChildWithName (midiMapType);
    for (int i = 0; i < map.getNumChildren(); ++i)
    {
        auto entry = map.getChild (i);
        const int cc = entry[midiMapCcProp];
        if (cc >= 0 && cc < (int) ccTargets.size())
            ccTargets[(size_t) cc].store (
                apvts.getParameter (entry[midiMapParamProp].toString()));
    }
}

// State ---------------------------------------------------------------------

void ZenithAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    commitPendingMidiLearn(); // runs on the message thread; flush before saving

    if (auto xml = stateManager.saveSession().createXml())
    {
        xml->setAttribute ("stateVersion", stateVersion);
        copyXmlToBinary (*xml, destData);
    }
}

bool ZenithAudioProcessor::applyStateXml (const juce::XmlElement& xml)
{
    if (xml.getIntAttribute ("stateVersion", 0) > stateVersion)
        return false; // written by a newer schema than this build understands

    // Session: both A/B slots + which is active.
    if (xml.hasTagName (stateManager.sessionType.toString()))
        return stateManager.loadSession (juce::ValueTree::fromXml (xml));

    // Bare preset state: one state, seeded into both slots.
    if (xml.hasTagName (apvts.state.getType()))
    {
        stateManager.loadLegacyState (juce::ValueTree::fromXml (xml));
        return true;
    }

    return false;
}

void ZenithAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        applyStateXml (*xml);
}

juce::AudioProcessorEditor* ZenithAudioProcessor::createEditor()
{
    return new ZenithAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ZenithAudioProcessor();
}
