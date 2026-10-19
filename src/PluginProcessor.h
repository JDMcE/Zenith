#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Parameters.h"
#include "StateManager.h"
#include "presets/PresetManager.h"
#include "dsp/LimiterEngine.h"
#include "dsp/LoudnessSuite.h"
#include "dsp/Dither.h"
#include "dsp/TruePeak.h"

class ZenithAudioProcessor : public juce::AudioProcessor
{
public:
    ZenithAudioProcessor();
    ~ZenithAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Zenith"; }
    bool acceptsMidi() const override { return true; }  // MIDI Learn (v0.6)
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Saved-state schema version, embedded in every state/preset we write.
    // States from a newer schema are ignored rather than half-applied (the
    // standalone app keeps stale state across dev builds). v1: session root
    // carries both A/B slots (the family scheme Meridian reached at its v2).
    static constexpr int stateVersion = 1;
    bool applyStateXml (const juce::XmlElement&);

    // MIDI Learn (v0.6, ported from Meridian) ---------------------------------
    // Arm on the message thread; the audio thread captures the next CC it
    // sees and starts applying it immediately. The mapping is persisted into
    // the state tree by commitPendingMidiLearn(), which must run on the
    // message thread — the editor timer calls it, and getStateInformation
    // calls it too so a headless session save still keeps the mapping.
    void armMidiLearn (const juce::String& paramId);
    void cancelMidiLearn() { learnTarget.store (nullptr); }
    bool isMidiLearnArmed() const { return learnTarget.load() != nullptr; }
    int getMappedCc (const juce::String& paramId) const;   // -1 if unmapped
    void clearMidiMapping (const juce::String& paramId);
    void commitPendingMidiLearn();

    juce::AudioProcessorValueTreeState apvts;

    // Snapshot-based undo/redo with independent history per A/B slot, plus
    // the preset ecosystem (both from jdm_shared; preset loads and name
    // changes route through the StateManager so they are undoable).
    znth::StateManager stateManager { *this, apvts, juce::Identifier ("ZenithSession") };
    znth::PresetManager presets { apvts, znth::makePresetConfig(),
        [this] (const juce::ValueTree& t) { stateManager.applyState (t); },
        [this] { return stateManager.getCurrentPresetName(); },
        [this] (const juce::String& n) { stateManager.setCurrentPresetName (n); } };

    // Metering values for the GUI. Gain reduction is the block maximum in dB
    // (positive = reducing); peaks are held since the last exchange (0).
    std::atomic<float> gainReductionDb { 0.0f };
    std::atomic<float> peakIn { 0.0f }, peakOut { 0.0f };

    // Smoothed processing load as a fraction of realtime (0..1+).
    std::atomic<float> cpuLoad { 0.0f };

    // BS.1770-4 / EBU R128 loudness of the output: momentary, short-term,
    // gated integrated, and LRA. The editor timer drives update().
    znth::LoudnessSuite loudness;

    // Lock-free level history for the main display: one frame per
    // scopeCellSamples host samples (max post-gain input, max output, block
    // GR in dB). The GUI reads frames between its last index and
    // scopeWriteIndex.
    struct ScopeFrame { float inPk, outPk, grDb; };
    static constexpr int scopeSize = 4096; // power of two
    static constexpr int scopeCellSamples = 256;
    std::array<ScopeFrame, scopeSize> scopeRing;
    std::atomic<uint32_t> scopeWriteIndex { 0 };

    // Inter-sample peak of the output (linear, held since the GUI's last
    // read). Runs regardless of True Peak mode so the meter can always show
    // dBTP; v0.4 puts it in the meter strip.
    znth::TruePeakMeter outTruePeak;

private:
    static constexpr int maxChans = znth::LimiterEngine::maxChannels;

    znth::LimiterEngine engine;

    // One oversampler per selectable factor (index 1..5 -> 2x..32x), all
    // linear-phase FIR and prepared up front so the factor can change at
    // block boundaries. Index 0 (Off) processes at the host rate. True Peak
    // mode raises the effective factor to at least 4x — the limiter must
    // run where inter-sample peaks are visible. The side-chain bus gets its
    // own bank (v0.5) built from the same filter design, so detector and
    // main stay sample-aligned at the oversampled rate (only the up-path is
    // used — the detector is never heard).
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 6> oversamplers;
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 6> scOversamplers;
    int currentOsApplied = -1;
    void applyOversamplingChoice();

    znth::DitherStage dither;

    juce::SmoothedValue<float> gainSmooth, outScaleSmooth, bypassMix;

    // DC offset filter (~5 Hz one-pole highpass), pre-gain, main bus only.
    std::array<float, maxChans> dcState {};
    float dcCoeff = 0.0f;

    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* ceilingParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* autoReleaseParam = nullptr;
    std::atomic<float>* attackParam = nullptr;
    std::atomic<float>* lookaheadParam = nullptr;
    std::atomic<float>* styleParam = nullptr;
    std::atomic<float>* linkTransientParam = nullptr;
    std::atomic<float>* linkReleaseParam = nullptr;
    std::atomic<float>* dcFilterParam = nullptr;
    std::atomic<float>* oversamplingParam = nullptr;
    std::atomic<float>* truePeakParam = nullptr;
    std::atomic<float>* ditherParam = nullptr;
    std::atomic<float>* noiseShapingParam = nullptr;
    std::atomic<float>* unityGainParam = nullptr;
    std::atomic<float>* auditionParam = nullptr;
    std::atomic<float>* extSidechainParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    juce::AudioBuffer<float> dryBuffer, delayedDryBuffer, scopeInBuffer;

    // Scope cell accumulation (audio thread only).
    float scopeAccIn = 0.0f, scopeAccOut = 0.0f, scopeAccGr = 0.0f;
    int scopeCellPos = 0;

    double hostSampleRate = 48000.0;
    int lastReportedLatency = -1;

    void updateLatency();

    // MIDI Learn plumbing. ccTargets is the audio-thread lookup (128 CCs);
    // the state tree child "MIDIMAP" is the persistent source of truth.
    std::array<std::atomic<juce::RangedAudioParameter*>, 128> ccTargets {};
    std::atomic<juce::RangedAudioParameter*> learnTarget { nullptr };
    std::atomic<juce::RangedAudioParameter*> pendingLearnParam { nullptr };
    std::atomic<int> pendingLearnCc { -1 };
    void rebuildMidiCcMap();   // message thread
    void handleMidi (juce::MidiBuffer&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZenithAudioProcessor)
};
