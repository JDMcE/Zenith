#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace znth
{

// Limiting styles. Order must match styleChoices (append-only to keep saved
// states stable). Each style maps onto attack-window / release ballistics /
// ceiling-margin traits — see LimiterEngine::styleTraits. v0.1 traits are a
// first pass; v0.2 tunes each into a genuinely distinct character.
enum class Style
{
    transparent = 0, // neutral reference: exactly the user's settings
    punch,           // short attack window, fast first-stage recovery
    dynamic,         // program-dependent release, transient-forward
    versatile,       // slightly softened all-rounder
    aggressive,      // fast everything, loudness-first
    modern,          // smooth dual-stage release, EDM/pop sheen
    bus,             // slow, fully linked, glue-like
    safe,            // maximum smoothing + extra ceiling margin
    numStyles
};

inline const juce::StringArray styleChoices {
    "Transparent", "Punch", "Dynamic", "Versatile", "Aggressive", "Modern", "Bus", "Safe"
};

// Oversampling factors (linear-phase FIR). Order must match oversamplingChoices.
// True Peak mode enforces an effective minimum of 4x so inter-sample peaks
// are actually visible to the limiter.
inline const juce::StringArray oversamplingChoices { "Off", "2x", "4x", "8x", "16x", "32x" };
constexpr int oversamplingFactors[] = { 1, 2, 4, 8, 16, 32 };
constexpr int minTruePeakOsChoice = 2; // index of 4x

// Output dither depth. Order must match ditherChoices.
inline const juce::StringArray ditherChoices { "Off", "24-bit", "16-bit" };
constexpr int ditherBits[] = { 0, 24, 16 };

// Noise shaping applied to the dither's quantization error. Order must
// match shapingChoices: Flat = plain TPDF, then the three shapers.
enum class NoiseShaping
{
    flat = 0,   // no shaping, plain TPDF
    slope,      // (1 - z^-1)^2 error feedback, simple HF slope
    fWeighted,  // Lipshitz 5-tap psychoacoustic F-weighting
    eWeighted   // Wannamaker 9-tap E-weighting
};

inline const juce::StringArray shapingChoices { "Flat", "Slope", "F-Weighted", "E-Weighted" };

// Parameter ranges ---------------------------------------------------------

constexpr float minGainDb = -30.0f, maxGainDb = 40.0f;
constexpr float minCeilingDb = -30.0f, maxCeilingDb = 0.0f;
constexpr float minReleaseMs = 10.0f, maxReleaseMs = 2500.0f;
constexpr float minAttackMs = 0.05f, maxAttackMs = 20.0f;
constexpr float minLookaheadMs = 0.1f, maxLookaheadMs = 20.0f;

// Parameter IDs ------------------------------------------------------------

namespace id
{
    inline const juce::String gain          { "gain" };
    inline const juce::String ceiling       { "ceiling" };
    inline const juce::String release       { "release" };
    inline const juce::String autoRelease   { "auto_release" };
    inline const juce::String attack        { "attack" };
    inline const juce::String lookahead     { "lookahead" };
    inline const juce::String style         { "style" };
    inline const juce::String linkTransient { "link_transient" };
    inline const juce::String linkRelease   { "link_release" };
    inline const juce::String dcFilter      { "dc_filter" };
    inline const juce::String oversampling  { "oversampling" };
    inline const juce::String truePeak      { "true_peak" };
    inline const juce::String dither        { "dither" };
    inline const juce::String noiseShaping  { "noise_shaping" };
    inline const juce::String unityGain     { "unity_gain" };
    inline const juce::String audition      { "audition" };
    inline const juce::String extSidechain  { "ext_sidechain" };
    inline const juce::String bypass        { "bypass" };
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using P = juce::AudioParameterFloat;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    auto dbAttr = juce::AudioParameterFloatAttributes().withLabel ("dB");
    auto msAttr = juce::AudioParameterFloatAttributes().withLabel ("ms");
    auto pcAttr = juce::AudioParameterFloatAttributes().withLabel ("%");

    params.push_back (std::make_unique<P> (
        juce::ParameterID (id::gain, 1), "Gain",
        juce::NormalisableRange<float> (minGainDb, maxGainDb, 0.01f), 0.0f, dbAttr));

    params.push_back (std::make_unique<P> (
        juce::ParameterID (id::ceiling, 1), "Output Ceiling",
        juce::NormalisableRange<float> (minCeilingDb, maxCeilingDb, 0.01f), -0.3f, dbAttr));

    params.push_back (std::make_unique<P> (
        juce::ParameterID (id::release, 1), "Release",
        juce::NormalisableRange<float> (minReleaseMs, maxReleaseMs, 0.1f, 0.3f), 100.0f, msAttr));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (id::autoRelease, 1), "Auto Release", false));

    // Attack is the smoothing window applied to the gain envelope. It is
    // clamped to the lookahead inside the engine — longer would break the
    // no-overshoot guarantee.
    params.push_back (std::make_unique<P> (
        juce::ParameterID (id::attack, 1), "Attack",
        juce::NormalisableRange<float> (minAttackMs, maxAttackMs, 0.01f, 0.4f), 2.0f, msAttr));

    params.push_back (std::make_unique<P> (
        juce::ParameterID (id::lookahead, 1), "Lookahead",
        juce::NormalisableRange<float> (minLookaheadMs, maxLookaheadMs, 0.01f, 0.4f), 2.0f, msAttr));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID (id::style, 1), "Style", styleChoices, (int) Style::transparent));

    params.push_back (std::make_unique<P> (
        juce::ParameterID (id::linkTransient, 1), "Transient Link",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, pcAttr));

    params.push_back (std::make_unique<P> (
        juce::ParameterID (id::linkRelease, 1), "Release Link",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, pcAttr));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (id::dcFilter, 1), "DC Filter", false));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID (id::oversampling, 1), "Oversampling",
        oversamplingChoices, 0));

    // True Peak mode: the limiter runs at >= 4x regardless of the
    // oversampling setting and the ceiling is honoured in dBTP.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (id::truePeak, 1), "True Peak", false));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID (id::dither, 1), "Dither", ditherChoices, 0));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID (id::noiseShaping, 1), "Noise Shaping",
        shapingChoices, (int) NoiseShaping::flat));

    // Unity Gain: undoes the input gain at the output so the limiting can be
    // judged at the incoming level.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (id::unityGain, 1), "Unity Gain", false));

    // Audition Limiting: monitor the difference between the (gained, delayed)
    // input and the limited output — what the limiter is taking away.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (id::audition, 1), "Audition Limiting", false));

    // External side chain (v0.5, feature 9): the side-chain input drives the
    // gain computation instead of the main input, so grouped stem instances
    // fed the same trigger duck identically. Falls back to internal
    // detection when the side-chain bus is disabled.
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (id::extSidechain, 1), "External Side Chain", false));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID (id::bypass, 1), "Bypass", false));

    return { params.begin(), params.end() };
}

} // namespace znth
