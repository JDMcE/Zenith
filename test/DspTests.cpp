// Offline DSP verification for Zenith.
//
// Instantiates the processor directly, drives test signals through it, and
// checks the limiter's contract: never over the ceiling, bit-exact below it,
// correct latency, sensible ballistics. Also covers the preset/state
// round-trips. Exits non-zero on failure.

#include "../src/PluginProcessor.h"
#include <cstdio>
#include <vector>

namespace
{

int failures = 0;

void expect (bool ok, const char* what, double got, double expected, double tol)
{
    std::printf ("%-62s got %9.4f  expected %9.4f (+/-%.4f)  %s\n",
                 what, got, expected, tol, ok ? "OK" : "FAIL");
    if (! ok)
        ++failures;
}

void expectNear (const char* what, double got, double expected, double tol)
{
    expect (std::abs (got - expected) <= tol, what, got, expected, tol);
}

void expectTrue (const char* what, bool ok)
{
    std::printf ("%-62s %s\n", what, ok ? "OK" : "FAIL");
    if (! ok)
        ++failures;
}

void setParam (ZenithAudioProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.apvts.getParameter (id);
    jassert (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

constexpr double sr = 48000.0;
constexpr int blockSize = 512;

std::unique_ptr<ZenithAudioProcessor> makeProcessor (int numChannels = 2)
{
    auto p = std::make_unique<ZenithAudioProcessor>();
    p->setPlayConfigDetails (numChannels, numChannels, sr, blockSize);
    p->prepareToPlay (sr, blockSize);
    return p;
}

// Runs numBlocks of a generator through the processor, returning the global
// output peak and (optionally) capturing all output samples.
template <typename Gen>
float runSignal (ZenithAudioProcessor& p, int numChannels, int numBlocks, Gen gen,
                 std::vector<float>* captureCh0 = nullptr)
{
    juce::AudioBuffer<float> buf (numChannels, blockSize);
    juce::MidiBuffer midi;
    float peak = 0.0f;
    long n = 0;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i, ++n)
        {
            const float v = gen (n);
            for (int ch = 0; ch < numChannels; ++ch)
                buf.setSample (ch, i, v);
        }
        p.processBlock (buf, midi);
        for (int ch = 0; ch < numChannels; ++ch)
            peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockSize));
        if (captureCh0 != nullptr)
            for (int i = 0; i < blockSize; ++i)
                captureCh0->push_back (buf.getSample (0, i));
    }
    return peak;
}

// --- Tests ------------------------------------------------------------------

// A quiet signal with no gain must come out bit-exact (delayed by the
// reported latency): the whole envelope path must idle at exactly 1.
void testBelowCeilingNull()
{
    auto p = makeProcessor();
    const int latency = p->getLatencySamples();

    std::vector<float> in, out;
    auto gen = [&in] (long n)
    {
        const float v = 0.25f * std::sin (0.01f * (float) n);
        in.push_back (v);
        return v;
    };
    runSignal (*p, 2, 40, gen, &out);
    in.resize (out.size());

    double maxDiff = 0.0;
    for (size_t i = (size_t) latency + 4096; i < out.size(); ++i)
        maxDiff = juce::jmax (maxDiff,
                              (double) std::abs (out[i] - in[i - (size_t) latency]));
    expectNear ("below-ceiling null (delay-aligned, bit-exact)", maxDiff, 0.0, 1.0e-7);
}

// The contract: no output sample above the ceiling, ever, however hard the
// input is driven — every style, gross overdrive, square-ish transients.
void testCeilingEnforcement()
{
    for (int style = 0; style < (int) znth::Style::numStyles; ++style)
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::gain, 20.0f);
        setParam (*p, znth::id::ceiling, -1.0f);
        setParam (*p, znth::id::style, (float) style);
        setParam (*p, znth::id::release, 50.0f);

        // Nasty signal: alternating bursts, clicks, and silence. Skip the
        // first 100 ms: the ceiling itself ramps from the default to -1 dB
        // over ~20 ms (smart parameter interpolation).
        auto gen = [] (long n)
        {
            if ((n / 4800) % 3 == 2) return 0.0f;
            if (n % 997 == 0) return 1.0f;
            return 0.9f * std::sin (0.09f * (float) n);
        };
        std::vector<float> out;
        runSignal (*p, 2, 60, gen, &out);
        float peak = 0.0f;
        for (size_t i = 4800; i < out.size(); ++i)
            peak = juce::jmax (peak, std::abs (out[i]));
        const float ceiling = juce::Decibels::decibelsToGain (-1.0f);

        char what[80];
        std::snprintf (what, sizeof (what), "ceiling never exceeded (style %s)",
                       znth::styleChoices[style].toRawUTF8());
        expectTrue (what, peak <= ceiling + 1.0e-6f);
    }
}

// +12 dB drive into a 0.5-amplitude sine peaks ~6.3 dB over the -0.3 dB
// ceiling: steady state must sit at the ceiling with matching reported GR.
void testSteadyStateGainReduction()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::gain, 12.0f);
    setParam (*p, znth::id::ceiling, -0.3f);
    setParam (*p, znth::id::release, 100.0f);

    std::vector<float> out;
    runSignal (*p, 2, 200,
               [] (long n) { return 0.5f * std::sin (0.06f * (float) n); }, &out);

    float steadyPeak = 0.0f;
    for (size_t i = out.size() - 4 * (size_t) blockSize; i < out.size(); ++i)
        steadyPeak = juce::jmax (steadyPeak, std::abs (out[i]));

    expectNear ("steady-state overdrive pinned to ceiling (dB)",
                juce::Decibels::gainToDecibels (steadyPeak), -0.3, 0.15);

    // GR = drive + sine peak dB - ceiling = 12 - 6.02 + 0.3.
    expectNear ("reported gain reduction (dB)",
                p->gainReductionDb.load(),
                12.0 + juce::Decibels::gainToDecibels (0.5) + 0.3, 1.0);
}

// Latency must equal the lookahead and track it when it changes.
void testLatencyReporting()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::lookahead, 2.0f);
    runSignal (*p, 2, 2, [] (long) { return 0.0f; });
    expectNear ("latency = 2 ms lookahead (samples)",
                p->getLatencySamples(), std::lround (sr * 0.002), 1.0);

    setParam (*p, znth::id::lookahead, 10.0f);
    runSignal (*p, 2, 2, [] (long) { return 0.0f; });
    expectNear ("latency tracks lookahead change to 10 ms",
                p->getLatencySamples(), std::lround (sr * 0.010), 1.0);
}

// After a loud burst ends, gain must recover toward unity at roughly the
// release time: a following quiet passage regains its level.
void testReleaseRecovery()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::gain, 12.0f);
    setParam (*p, znth::id::release, 50.0f);

    // 0.5 s of loud, then quiet sine at -30 dB (pre-gain).
    const long burstEnd = (long) (sr / 2.0);
    std::vector<float> out;
    runSignal (*p, 2, 200, [burstEnd] (long n)
    {
        const float amp = n < burstEnd ? 0.9f : 0.0316f;
        return amp * std::sin (0.07f * (float) n);
    }, &out);

    // ~10 release times after the burst, the quiet passage should be within
    // 0.5 dB of its nominal (gained) level.
    const size_t checkFrom = (size_t) burstEnd + (size_t) (sr * 0.6);
    float peak = 0.0f;
    for (size_t i = checkFrom; i < checkFrom + 24000 && i < out.size(); ++i)
        peak = juce::jmax (peak, std::abs (out[i]));

    const double expected = juce::Decibels::gainToDecibels (0.0316f) + 12.0;
    expectNear ("release recovers quiet passage after burst (dB)",
                juce::Decibels::gainToDecibels (peak), expected, 0.5);
}

// With transient + release link at 0%, a loud left channel must not duck a
// quiet right channel; at 100% it must.
void testChannelLinking()
{
    for (const bool linked : { false, true })
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::gain, 12.0f);
        setParam (*p, znth::id::linkTransient, linked ? 100.0f : 0.0f);
        setParam (*p, znth::id::linkRelease, linked ? 100.0f : 0.0f);

        juce::AudioBuffer<float> buf (2, blockSize);
        juce::MidiBuffer midi;
        float rightPeak = 0.0f;
        long n = 0;

        for (int b = 0; b < 120; ++b)
        {
            for (int i = 0; i < blockSize; ++i, ++n)
            {
                buf.setSample (0, i, 0.9f * std::sin (0.08f * (float) n));   // slams
                buf.setSample (1, i, 0.05f * std::sin (0.05f * (float) n));  // quiet
            }
            p->processBlock (buf, midi);
            if (b > 60)
                rightPeak = juce::jmax (rightPeak, buf.getMagnitude (1, 0, blockSize));
        }

        const double rightDb = juce::Decibels::gainToDecibels (rightPeak);
        const double nominal = juce::Decibels::gainToDecibels (0.05f) + 12.0;
        if (linked)
            expectTrue ("100% link ducks the quiet channel", rightDb < nominal - 3.0);
        else
            expectNear ("0% link leaves the quiet channel untouched (dB)",
                        rightDb, nominal, 0.5);
    }
}

// DC filter: a big offset must vanish; disabled, it must pass.
void testDcFilter()
{
    for (const bool on : { false, true })
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::dcFilter, on ? 1.0f : 0.0f);
        setParam (*p, znth::id::ceiling, 0.0f);

        std::vector<float> out;
        runSignal (*p, 2, 200, [] (long) { return 0.4f; }, &out);

        double mean = 0.0;
        for (size_t i = out.size() - 24000; i < out.size(); ++i)
            mean += out[i];
        mean /= 24000.0;

        if (on)
            expectNear ("DC filter removes 0.4 offset", mean, 0.0, 0.01);
        else
            expectNear ("DC passes when filter is off", mean, 0.4, 0.01);
    }
}

// Unity Gain: with +12 dB drive and unity on, a below-threshold signal comes
// out at the *input* level (limiting audible, loudness not).
void testUnityGain()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::gain, 12.0f);
    setParam (*p, znth::id::unityGain, 1.0f);

    std::vector<float> out;
    runSignal (*p, 2, 100,
               [] (long n) { return 0.05f * std::sin (0.05f * (float) n); }, &out);

    float peak = 0.0f;
    for (size_t i = out.size() - 24000; i < out.size(); ++i)
        peak = juce::jmax (peak, std::abs (out[i]));

    expectNear ("unity gain restores input level (dB)",
                juce::Decibels::gainToDecibels (peak),
                juce::Decibels::gainToDecibels (0.05f), 0.3);
}

// Delta audition: below the ceiling the delta is silence; over it, it isn't.
void testDeltaAudition()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::audition, 1.0f);

    const float quietPeak = runSignal (*p, 2, 40,
        [] (long n) { return 0.2f * std::sin (0.05f * (float) n); });
    expectNear ("delta is silent below the ceiling", quietPeak, 0.0, 1.0e-5);

    auto p2 = makeProcessor();
    setParam (*p2, znth::id::audition, 1.0f);
    setParam (*p2, znth::id::gain, 12.0f);
    const float loudPeak = runSignal (*p2, 2, 40,
        [] (long n) { return 0.9f * std::sin (0.05f * (float) n); });
    expectTrue ("delta carries the removed material when limiting",
                loudPeak > 0.05f);
}

// Bypass must null against the raw input (no delay: the crossfade path).
void testBypassNull()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::gain, 12.0f);
    setParam (*p, znth::id::bypass, 1.0f);

    std::vector<float> in, out;
    runSignal (*p, 2, 40, [&in] (long n)
    {
        const float v = 0.5f * std::sin (0.03f * (float) n);
        in.push_back (v);
        return v;
    }, &out);
    in.resize (out.size());

    double maxDiff = 0.0;
    for (size_t i = 4096; i < out.size(); ++i)
        maxDiff = juce::jmax (maxDiff, (double) std::abs (out[i] - in[i]));
    expectNear ("bypass null against raw input", maxDiff, 0.0, 1.0e-6);
}

void testMonoPath()
{
    auto p = makeProcessor (1);
    setParam (*p, znth::id::gain, 20.0f);
    setParam (*p, znth::id::ceiling, -0.5f);

    std::vector<float> out;
    runSignal (*p, 1, 60,
               [] (long n) { return 0.9f * std::sin (0.06f * (float) n); }, &out);
    float peak = 0.0f;
    for (size_t i = 4800; i < out.size(); ++i) // skip the ceiling ramp-in
        peak = juce::jmax (peak, std::abs (out[i]));
    expectTrue ("mono path respects ceiling",
                peak <= juce::Decibels::decibelsToGain (-0.5f) + 1.0e-6f);
}

// v0.2 style characters: identical programme through every style must give
// audibly distinct results with the documented ballistics orderings, and
// Safe must keep its extra ceiling margin.
void testStyleCharacters()
{
    // Burst train: 100 ms loud, 150 ms quiet, sine carrier, +10 dB drive.
    auto burstGen = [] (long n)
    {
        const float amp = (n % 12000) < 4800 ? 0.9f : 0.15f;
        return amp * std::sin (0.05f * (float) n);
    };

    std::array<std::vector<float>, (size_t) znth::Style::numStyles> outs;
    for (int style = 0; style < (int) znth::Style::numStyles; ++style)
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::gain, 10.0f);
        setParam (*p, znth::id::release, 100.0f);
        setParam (*p, znth::id::style, (float) style);
        runSignal (*p, 2, 240, burstGen, &outs[(size_t) style]);
    }

    // RMS over the quiet segments (skipping the first 0.5 s and the segment
    // transitions): a faster release recovers more level there.
    auto quietRms = [] (const std::vector<float>& v)
    {
        double sum = 0.0;
        long count = 0;
        for (size_t i = 24000; i < v.size(); ++i)
        {
            const long phase = (long) i % 12000;
            if (phase >= 5200 && phase < 11800)
            {
                sum += (double) v[i] * v[i];
                ++count;
            }
        }
        return std::sqrt (sum / (double) count);
    };

    const double transparent = quietRms (outs[(size_t) znth::Style::transparent]);
    expectTrue ("Aggressive recovers faster than Transparent",
                quietRms (outs[(size_t) znth::Style::aggressive]) > transparent * 1.01);
    expectTrue ("Punch recovers faster than Transparent",
                quietRms (outs[(size_t) znth::Style::punch]) > transparent * 1.005);
    expectTrue ("Bus recovers slower than Transparent",
                quietRms (outs[(size_t) znth::Style::bus]) < transparent * 0.998);

    // Pairwise distinctness: every style pair differs by a clearly-nonzero
    // relative RMS on the same programme (audible-difference proxy).
    double refRms = 0.0;
    for (size_t i = 24000; i < outs[0].size(); ++i)
        refRms += (double) outs[0][i] * outs[0][i];
    refRms = std::sqrt (refRms / (double) (outs[0].size() - 24000));

    for (int a = 0; a < (int) znth::Style::numStyles; ++a)
        for (int b = a + 1; b < (int) znth::Style::numStyles; ++b)
        {
            double diff = 0.0;
            for (size_t i = 24000; i < outs[(size_t) a].size(); ++i)
            {
                const double d = (double) outs[(size_t) a][i] - outs[(size_t) b][i];
                diff += d * d;
            }
            diff = std::sqrt (diff / (double) (outs[(size_t) a].size() - 24000));

            char what[80];
            std::snprintf (what, sizeof (what), "styles distinct: %s vs %s",
                           znth::styleChoices[a].toRawUTF8(),
                           znth::styleChoices[b].toRawUTF8());
            expectTrue (what, diff / refRms > 0.001);
        }

    // Safe's 0.1 dB extra margin below the ceiling.
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::gain, 10.0f);
        setParam (*p, znth::id::style, (float) znth::Style::safe);
        std::vector<float> out;
        runSignal (*p, 2, 100,
                   [] (long n) { return 0.9f * std::sin (0.05f * (float) n); }, &out);
        float steadyPeak = 0.0f;
        for (size_t i = out.size() - 24000; i < out.size(); ++i)
            steadyPeak = juce::jmax (steadyPeak, std::abs (out[i]));
        expectTrue ("Safe style keeps its extra ceiling margin",
                    juce::Decibels::gainToDecibels (steadyPeak) <= -0.38);
    }
}

// v0.2 smart parameter interpolation: hammering gain/ceiling/link with new
// random targets every block must stay click-free and inside the ceiling.
void testParameterInterpolation()
{
    auto p = makeProcessor();
    juce::Random rng (42);

    juce::AudioBuffer<float> buf (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> out;
    long n = 0;

    for (int b = 0; b < 200; ++b)
    {
        setParam (*p, znth::id::gain, rng.nextFloat() * 12.0f);
        setParam (*p, znth::id::ceiling, -1.0f - rng.nextFloat() * 5.0f);
        setParam (*p, znth::id::linkTransient, rng.nextFloat() * 100.0f);
        setParam (*p, znth::id::linkRelease, rng.nextFloat() * 100.0f);

        for (int i = 0; i < blockSize; ++i, ++n)
        {
            const float v = 0.9f * std::sin (0.02f * (float) n);
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        p->processBlock (buf, midi);
        for (int i = 0; i < blockSize; ++i)
            out.push_back (buf.getSample (0, i));
    }

    double maxDelta = 0.0;
    float peak = 0.0f;
    for (size_t i = 4801; i < out.size(); ++i)
    {
        maxDelta = juce::jmax (maxDelta, (double) std::abs (out[i] - out[i - 1]));
        peak = juce::jmax (peak, std::abs (out[i]));
    }

    expectTrue ("random per-block automation stays click-free (delta < 0.08)",
                maxDelta < 0.08);
    expectTrue ("automated ceiling still never exceeded",
                peak <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-5f);
}

// v0.3 oversampling: the ceiling contract must hold at every factor, the
// below-ceiling path must stay transparent (linear-phase FIR ripple only),
// and the reported latency must match where an impulse actually lands.
void testOversampling()
{
    // Ceiling enforcement across factors, two contrasting styles.
    for (const int osChoice : { 1, 2, 5 }) // 2x, 4x, 32x
        for (const int style : { (int) znth::Style::transparent,
                                 (int) znth::Style::aggressive })
        {
            auto p = makeProcessor();
            setParam (*p, znth::id::gain, 18.0f);
            setParam (*p, znth::id::ceiling, -1.0f);
            setParam (*p, znth::id::style, (float) style);
            setParam (*p, znth::id::oversampling, (float) osChoice);

            std::vector<float> out;
            runSignal (*p, 2, 40, [] (long n)
            {
                if (n % 997 == 0) return 1.0f;
                return 0.8f * std::sin (0.11f * (float) n);
            }, &out);

            float peak = 0.0f;
            for (size_t i = 4800; i < out.size(); ++i)
                peak = juce::jmax (peak, std::abs (out[i]));

            char what[96];
            std::snprintf (what, sizeof (what),
                           "ceiling holds at %s oversampling (style %s)",
                           znth::oversamplingChoices[osChoice].toRawUTF8(),
                           znth::styleChoices[style].toRawUTF8());
            // Downsampling a limited waveform can ring slightly (Gibbs);
            // 0.05 dB is the accepted bound for the sample-peak path.
            const double db = juce::Decibels::gainToDecibels (peak);
            expect (db <= -1.0 + 0.05, what, db, -1.0, 0.05);
        }

    // Transparency below the ceiling at 4x: output nulls against the
    // delay-aligned input to FIR-ripple accuracy.
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::oversampling, 2.0f);
        runSignal (*p, 2, 2, [] (long) { return 0.0f; }); // settle the switch
        const int latency = p->getLatencySamples();

        std::vector<float> in, out;
        runSignal (*p, 2, 40, [&in] (long n)
        {
            const float v = 0.25f * std::sin (0.03f * (float) n);
            in.push_back (v);
            return v;
        }, &out);
        in.resize (out.size());

        // The true 4x FIR latency is fractional and we report it rounded
        // (deliberately - see the oversampler setup comment), so the null
        // carries a half-sample skew: bound = amp * omega * 0.5 plus FIR
        // ripple. For 0.25 amplitude at 0.03 rad/sample that is ~0.004.
        double maxDiff = 0.0;
        for (size_t i = (size_t) latency + 9600; i < out.size(); ++i)
            maxDiff = juce::jmax (maxDiff,
                                  (double) std::abs (out[i] - in[i - (size_t) latency]));
        expectNear ("4x oversampled path transparent below ceiling", maxDiff, 0.0, 6.0e-3);
    }

    // Latency reporting: an impulse lands exactly at the reported latency.
    for (const int osChoice : { 0, 2, 4 }) // Off, 4x, 16x
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::oversampling, (float) osChoice);
        runSignal (*p, 2, 2, [] (long) { return 0.0f; });
        const int latency = p->getLatencySamples();

        std::vector<float> out;
        runSignal (*p, 2, 8, [] (long n) { return n == 256 ? 0.5f : 0.0f; }, &out);

        int argmax = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::abs (out[i]) > std::abs (out[(size_t) argmax]))
                argmax = (int) i;

        char what[80];
        std::snprintf (what, sizeof (what), "impulse lands at reported latency (%s)",
                       znth::oversamplingChoices[osChoice].toRawUTF8());
        expectNear (what, argmax - 256, latency, 1.0);
    }

    // DC filter still works through the oversampled path.
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::dcFilter, 1.0f);
        setParam (*p, znth::id::ceiling, 0.0f);
        setParam (*p, znth::id::oversampling, 2.0f);

        std::vector<float> out;
        runSignal (*p, 2, 200, [] (long) { return 0.4f; }, &out);
        double mean = 0.0;
        for (size_t i = out.size() - 24000; i < out.size(); ++i)
            mean += out[i];
        expectNear ("DC filter removes offset at 4x oversampling",
                    mean / 24000.0, 0.0, 0.01);
    }
}

// v0.3 true peak: a sine at fs/4 with 45-degree phase has sample peaks
// 3 dB below its true peak — the classic inter-sample-peak trap.
void testTruePeak()
{
    auto ispGen = [] (long n)
    {
        return 1.0f * std::sin (juce::MathConstants<float>::halfPi * (float) n
                                + juce::MathConstants<float>::pi / 4.0f);
    };

    // The meter itself: sample peak reads ~0.707, true peak ~1.0.
    {
        znth::TruePeakMeter meter;
        meter.prepare (1);
        std::vector<float> sig;
        for (long n = 0; n < 4096; ++n)
            sig.push_back (ispGen (n));
        meter.process (sig.data(), nullptr, (int) sig.size());
        expectNear ("true-peak meter sees through the fs/4 trap (dBTP)",
                    juce::Decibels::gainToDecibels (meter.readAndReset()), 0.0, 0.2);
    }

    // Without True Peak (no oversampling), the sample-peak limiter is blind
    // to the ISP: output true peak sails over the -1 dB ceiling.
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::ceiling, -1.0f);
        std::vector<float> out;
        runSignal (*p, 2, 60, ispGen, &out);

        znth::TruePeakMeter meter;
        meter.prepare (1);
        meter.process (out.data() + 4800, nullptr, (int) out.size() - 4800);
        const double db = juce::Decibels::gainToDecibels (meter.readAndReset());
        expect (db > -0.5, "without True Peak the ISP passes (> -0.5 dBTP)", db, 0.0, 0.5);
    }

    // With True Peak on (auto >= 4x), the output true peak honours the
    // ceiling in dBTP.
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::ceiling, -1.0f);
        setParam (*p, znth::id::truePeak, 1.0f);
        std::vector<float> out;
        runSignal (*p, 2, 60, ispGen, &out);

        znth::TruePeakMeter meter;
        meter.prepare (1);
        meter.process (out.data() + 4800, nullptr, (int) out.size() - 4800);
        const double db = juce::Decibels::gainToDecibels (meter.readAndReset());
        expect (db <= -1.0 + 0.1, "True Peak mode holds the ceiling in dBTP", db, -1.0, 0.1);
    }
}

// v0.3 dither: quantized output sits exactly on the target grid, and the
// shaped error spectrum moves energy out of the audible low band.
void testDither()
{
    auto measureLfError = [] (znth::NoiseShaping shaping)
    {
        auto p = makeProcessor();
        setParam (*p, znth::id::dither, 2.0f); // 16-bit
        setParam (*p, znth::id::noiseShaping, (float) (int) shaping);
        const int latency = p->getLatencySamples();

        std::vector<float> in, out;
        runSignal (*p, 2, 100, [&in] (long n)
        {
            const float v = 0.1f * std::sin (0.026f * (float) n); // ~200 Hz
            in.push_back (v);
            return v;
        }, &out);
        in.resize (out.size());

        // Quantization-grid check (once, on the flat run).
        if (shaping == znth::NoiseShaping::flat)
        {
            const float q = 1.0f / 32768.0f;
            double worst = 0.0;
            for (size_t i = out.size() - 8192; i < out.size(); ++i)
            {
                const float steps = out[i] / q;
                worst = juce::jmax (worst, (double) std::abs (steps - std::round (steps)));
            }
            expectNear ("16-bit dithered output sits exactly on the grid", worst, 0.0, 1.0e-3);
        }

        // Error signal = out - aligned in; Goertzel the band up to ~3 kHz.
        double lfPower = 0.0;
        const size_t start = (size_t) latency + 9600;
        const int len = 32768;
        for (double freq = 250.0; freq <= 3000.0; freq += 250.0)
        {
            const double w = juce::MathConstants<double>::twoPi * freq / sr;
            double s0 = 0.0, s1 = 0.0, s2 = 0.0;
            const double coeff = 2.0 * std::cos (w);
            for (int i = 0; i < len; ++i)
            {
                const double e = out[start + (size_t) i] - in[start + (size_t) i - (size_t) latency];
                s0 = e + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            lfPower += s1 * s1 + s2 * s2 - coeff * s1 * s2;
        }
        return lfPower;
    };

    const double flat = measureLfError (znth::NoiseShaping::flat);
    const double slope = measureLfError (znth::NoiseShaping::slope);
    const double fw = measureLfError (znth::NoiseShaping::fWeighted);
    const double ew = measureLfError (znth::NoiseShaping::eWeighted);

    expectTrue ("Slope shaping cuts low-band dither error vs flat", slope < flat * 0.5);
    expectTrue ("F-weighted shaping cuts low-band dither error vs flat", fw < flat * 0.5);
    expectTrue ("E-weighted shaping cuts low-band dither error vs flat", ew < flat * 0.5);

    // Dither off: below the ceiling the path stays bit-exact (not on grid).
    {
        auto p = makeProcessor();
        const int latency = p->getLatencySamples();
        std::vector<float> in, out;
        runSignal (*p, 2, 20, [&in] (long n)
        {
            const float v = 0.123456f * std::sin (0.037f * (float) n);
            in.push_back (v);
            return v;
        }, &out);
        in.resize (out.size());
        double maxDiff = 0.0;
        for (size_t i = (size_t) latency + 4096; i < out.size(); ++i)
            maxDiff = juce::jmax (maxDiff,
                                  (double) std::abs (out[i] - in[i - (size_t) latency]));
        expectNear ("dither off leaves the path bit-exact", maxDiff, 0.0, 1.0e-7);
    }
}

// v0.3 low-CPU audit: informational benchmark table (fraction of realtime,
// Release build), with a sanity bound at 4x.
void benchmarkStylesAndFactors()
{
    std::printf ("\nCPU benchmark (fraction of realtime, 1 s stereo @ 48 kHz):\n");
    juce::Random rng (7);
    std::vector<float> noise ((size_t) sr);
    for (auto& v : noise)
        v = rng.nextFloat() * 1.8f - 0.9f;

    for (const int osChoice : { 0, 2, 5 })
    {
        std::printf ("  %-4s:", znth::oversamplingChoices[osChoice].toRawUTF8());
        for (const int style : { (int) znth::Style::transparent,
                                 (int) znth::Style::aggressive,
                                 (int) znth::Style::safe })
        {
            auto p = makeProcessor();
            setParam (*p, znth::id::gain, 8.0f);
            setParam (*p, znth::id::style, (float) style);
            setParam (*p, znth::id::oversampling, (float) osChoice);
            runSignal (*p, 2, 2, [] (long) { return 0.0f; }); // settle

            juce::AudioBuffer<float> buf (2, blockSize);
            juce::MidiBuffer midi;
            const auto t0 = juce::Time::getHighResolutionTicks();
            for (size_t pos = 0; pos + blockSize <= noise.size(); pos += blockSize)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    buf.setSample (0, i, noise[pos + (size_t) i]);
                    buf.setSample (1, i, noise[pos + (size_t) i]);
                }
                p->processBlock (buf, midi);
            }
            const double secs = juce::Time::highResolutionTicksToSeconds (
                juce::Time::getHighResolutionTicks() - t0);
            std::printf ("  %s %.3f", znth::styleChoices[style].toRawUTF8(), secs);

            if (osChoice == 2 && style == (int) znth::Style::transparent)
                expectTrue ("\n4x Transparent runs under realtime", secs < 1.0);
        }
        std::printf ("\n");
    }
}

// v0.4 loudness suite: BS.1770-4 calibration, absolute + relative gating,
// and EBU Tech 3342 loudness range.
void testLoudnessSuite()
{
    // Stereo sine feeder; both channels identical, so the summed mean
    // square is amp^2 and LUFS = 20 log10 (amp): the -0.691 offset exists
    // to cancel the K-filter's +0.69 dB gain at 997 Hz (BS.1770 calibrates
    // a full-scale single-channel 997 Hz sine to -3.01 LKFS). The filter
    // parameterisation lands within ~0.3 dB of that at 48 kHz.
    auto feed = [] (jdm::LoudnessSuite& s, double seconds, float amp, long& n)
    {
        std::vector<float> buf (4800);
        const double inc = juce::MathConstants<double>::twoPi * 997.0 / sr;
        long remaining = (long) (seconds * sr);
        while (remaining > 0)
        {
            const int chunk = (int) juce::jmin ((long) buf.size(), remaining);
            for (int i = 0; i < chunk; ++i, ++n)
                buf[(size_t) i] = amp * (float) std::sin (inc * (double) n);
            s.process (buf.data(), buf.data(), chunk);
            s.update(); // drain as we go (the ring holds ~51 s)
            remaining -= chunk;
        }
    };

    const double lufsFor = [] (float amp)
    {
        return 20.0 * std::log10 ((double) amp);
    } (0.5f); // -6.02 for the calibration runs

    // Calibration: steady 997 Hz at 0.5 -> M = S = I, LRA ~ 0.
    {
        jdm::LoudnessSuite suite;
        suite.prepare (sr, 2);
        long n = 0;
        feed (suite, 8.0, 0.5f, n);
        suite.computeGatedNow();

        expectNear ("loudness momentary at 997 Hz calibration (LUFS)",
                    suite.momentaryLufs(), lufsFor, 0.35);
        expectNear ("loudness short-term matches", suite.shortTermLufs(), lufsFor, 0.35);
        expectNear ("loudness integrated matches", suite.integratedLufs(), lufsFor, 0.35);
        expectNear ("steady signal has ~zero LRA", suite.loudnessRangeLu(), 0.0, 0.5);
    }

    // Absolute gate: trailing silence must not drag the integrated down.
    {
        jdm::LoudnessSuite suite;
        suite.prepare (sr, 2);
        long n = 0;
        feed (suite, 8.0, 0.5f, n);
        feed (suite, 12.0, 0.0f, n);
        suite.computeGatedNow();
        expectNear ("integrated ignores silence (absolute gate)",
                    suite.integratedLufs(), lufsFor, 0.4);
    }

    // Relative gate: a passage 35 LU down (well above the -70 absolute
    // gate) must also be excluded.
    {
        jdm::LoudnessSuite suite;
        suite.prepare (sr, 2);
        long n = 0;
        feed (suite, 8.0, 0.5f, n);
        feed (suite, 12.0, 0.5f * 0.01778f, n); // -35 dB relative
        suite.computeGatedNow();
        expectNear ("integrated ignores quiet passage (relative gate)",
                    suite.integratedLufs(), lufsFor, 0.6);
    }

    // LRA: 10 s at -6.7 then 10 s at -16.7 -> range ~10 LU.
    {
        jdm::LoudnessSuite suite;
        suite.prepare (sr, 2);
        long n = 0;
        feed (suite, 10.0, 0.5f, n);
        feed (suite, 10.0, 0.158f, n);
        suite.computeGatedNow();
        expectNear ("LRA of a 10 LU bimodal programme",
                    suite.loudnessRangeLu(), 10.0, 2.5);
    }
}

// --- v0.5: routing & formats breadth ----------------------------------------

// A processor on a given main layout, optionally with the stereo side-chain
// bus enabled.
std::unique_ptr<ZenithAudioProcessor> makeBusProcessor (const juce::AudioChannelSet& mainSet,
                                                        bool withSidechain = false)
{
    auto p = std::make_unique<ZenithAudioProcessor>();
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (mainSet);
    layout.inputBuses.add (withSidechain ? juce::AudioChannelSet::stereo()
                                         : juce::AudioChannelSet::disabled());
    layout.outputBuses.add (mainSet);
    expectTrue ("bus layout accepted", p->setBusesLayout (layout));
    p->setRateAndBufferSizeDetails (sr, blockSize);
    p->prepareToPlay (sr, blockSize);
    return p;
}

// Surround (feature 18): the ceiling contract and the below-ceiling null
// must hold on every channel of 5.1 and 7.1, exactly as they do in stereo.
void testSurroundLayouts()
{
    const juce::AudioChannelSet sets[] = { juce::AudioChannelSet::create5point1(),
                                           juce::AudioChannelSet::create7point1() };
    for (const auto& set : sets)
    {
        auto p = makeBusProcessor (set);
        setParam (*p, znth::id::gain, 20.0f);
        setParam (*p, znth::id::ceiling, -1.0f);

        const int nCh = set.size();
        juce::AudioBuffer<float> buf (nCh, blockSize);
        juce::MidiBuffer midi;
        float peak = 0.0f;
        long n = 0;

        for (int b = 0; b < 80; ++b)
        {
            for (int i = 0; i < blockSize; ++i, ++n)
            {
                // Burst train with square-ish edges on every channel.
                const bool on = (n / 2400) % 2 == 0;
                const float v = on ? 0.95f * (std::sin (0.09f * (float) n) > 0.0f ? 1.0f : -0.7f)
                                   : 0.02f * std::sin (0.03f * (float) n);
                for (int ch = 0; ch < nCh; ++ch)
                    buf.setSample (ch, i, v);
            }
            p->processBlock (buf, midi);
            for (int ch = 0; ch < nCh; ++ch)
                peak = juce::jmax (peak, buf.getMagnitude (ch, 0, blockSize));
        }

        char what[96];
        std::snprintf (what, sizeof (what), "%d-channel overdrive stays under -1 dB ceiling",
                       nCh);
        expectTrue (what, peak <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-4f);
    }

    // Below-ceiling null on 5.1: bit-exact on every channel (incl. LFE).
    {
        auto p = makeBusProcessor (juce::AudioChannelSet::create5point1());
        const int latency = p->getLatencySamples();

        juce::AudioBuffer<float> buf (6, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> in;
        std::vector<std::array<float, 6>> out;
        long n = 0;

        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < blockSize; ++i, ++n)
            {
                const float v = 0.25f * std::sin (0.01f * (float) n);
                in.push_back (v);
                for (int ch = 0; ch < 6; ++ch)
                    buf.setSample (ch, i, v);
            }
            p->processBlock (buf, midi);
            for (int i = 0; i < blockSize; ++i)
            {
                std::array<float, 6> frame;
                for (int ch = 0; ch < 6; ++ch)
                    frame[(size_t) ch] = buf.getSample (ch, i);
                out.push_back (frame);
            }
        }

        double maxDiff = 0.0;
        for (size_t i = (size_t) latency + 4096; i < out.size(); ++i)
            for (int ch = 0; ch < 6; ++ch)
                maxDiff = juce::jmax (maxDiff,
                    (double) std::abs (out[i][(size_t) ch] - in[i - (size_t) latency]));
        expectNear ("5.1 below-ceiling null (all channels, delay-aligned)",
                    maxDiff, 0.0, 1.0e-7);
    }
}

// Surround link topology: the full-range channels move as one group; the
// LFE is decoupled in both directions yet still limited itself.
void testSurroundLinkTopology()
{
    // 5.1 channel order: L R C LFE Ls Rs.
    constexpr int lfe = 3;

    // Case 1: L slams -> C ducks (group link), LFE stays untouched.
    {
        auto p = makeBusProcessor (juce::AudioChannelSet::create5point1());
        setParam (*p, znth::id::gain, 12.0f);

        juce::AudioBuffer<float> buf (6, blockSize);
        juce::MidiBuffer midi;
        float centerPeak = 0.0f, lfePeak = 0.0f;
        long n = 0;

        for (int b = 0; b < 120; ++b)
        {
            buf.clear();
            for (int i = 0; i < blockSize; ++i, ++n)
            {
                buf.setSample (0, i, 0.9f * std::sin (0.08f * (float) n));    // slams
                buf.setSample (2, i, 0.05f * std::sin (0.05f * (float) n));   // quiet C
                buf.setSample (lfe, i, 0.05f * std::sin (0.02f * (float) n)); // quiet LFE
            }
            p->processBlock (buf, midi);
            if (b > 60)
            {
                centerPeak = juce::jmax (centerPeak, buf.getMagnitude (2, 0, blockSize));
                lfePeak = juce::jmax (lfePeak, buf.getMagnitude (lfe, 0, blockSize));
            }
        }

        const double nominal = juce::Decibels::gainToDecibels (0.05f) + 12.0;
        expectTrue ("surround link ducks the quiet centre with the mains",
                    juce::Decibels::gainToDecibels (centerPeak) < nominal - 3.0);
        expectNear ("LFE is not ducked by the mains (dB)",
                    juce::Decibels::gainToDecibels (lfePeak), nominal, 0.5);
    }

    // Case 2: LFE slams -> the mains stay untouched, the LFE itself is
    // still held at the ceiling.
    {
        auto p = makeBusProcessor (juce::AudioChannelSet::create5point1());
        setParam (*p, znth::id::gain, 12.0f);

        juce::AudioBuffer<float> buf (6, blockSize);
        juce::MidiBuffer midi;
        float leftPeak = 0.0f, lfePeak = 0.0f;
        long n = 0;

        for (int b = 0; b < 120; ++b)
        {
            buf.clear();
            for (int i = 0; i < blockSize; ++i, ++n)
            {
                buf.setSample (lfe, i, 0.9f * std::sin (0.03f * (float) n));  // slams
                buf.setSample (0, i, 0.05f * std::sin (0.05f * (float) n));   // quiet L
            }
            p->processBlock (buf, midi);
            if (b > 60)
            {
                leftPeak = juce::jmax (leftPeak, buf.getMagnitude (0, 0, blockSize));
                lfePeak = juce::jmax (lfePeak, buf.getMagnitude (lfe, 0, blockSize));
            }
        }

        const double nominal = juce::Decibels::gainToDecibels (0.05f) + 12.0;
        expectNear ("a slamming LFE does not duck the mains (dB)",
                    juce::Decibels::gainToDecibels (leftPeak), nominal, 0.5);
        expectTrue ("the LFE itself is still limited to the ceiling",
                    lfePeak <= juce::Decibels::decibelsToGain (-0.3f) + 1.0e-4f);
    }
}

// External side chain (feature 9): the detector follows the side-chain
// input, instances sharing a trigger duck identically, and the ceiling
// contract survives a side chain quieter than the mains.
void testExternalSidechain()
{
    juce::MidiBuffer midi;

    // Main and SC carry the same waveform at different levels, so the
    // expected steady output peak is ceiling * (mainAmp / scAmp).
    auto runScSignal = [&midi] (ZenithAudioProcessor& p, float mainAmp, float scAmp,
                                std::vector<float>* capture = nullptr)
    {
        juce::AudioBuffer<float> buf (4, blockSize);
        float peak = 0.0f;
        long n = 0;
        for (int b = 0; b < 60; ++b)
        {
            for (int i = 0; i < blockSize; ++i, ++n)
            {
                const float v = std::sin (0.07f * (float) n);
                buf.setSample (0, i, mainAmp * v);
                buf.setSample (1, i, mainAmp * v);
                buf.setSample (2, i, scAmp * v);
                buf.setSample (3, i, scAmp * v);
            }
            p.processBlock (buf, midi);
            if (b > 20)
                peak = juce::jmax (peak, buf.getMagnitude (0, 0, blockSize));
            if (capture != nullptr)
                for (int i = 0; i < blockSize; ++i)
                    capture->push_back (buf.getSample (0, i));
        }
        return peak;
    };

    // Detector follows the side chain: a -20 dB main over a hot trigger
    // comes out ducked to ceiling * ratio.
    {
        auto p = makeBusProcessor (juce::AudioChannelSet::stereo(), true);
        setParam (*p, znth::id::extSidechain, 1.0f);
        setParam (*p, znth::id::ceiling, -6.0f);
        const float peak = runScSignal (*p, 0.1f, 0.9f);
        expectNear ("ext SC ducks the main by the trigger's GR (dB)",
                    juce::Decibels::gainToDecibels (peak),
                    -6.0 + juce::Decibels::gainToDecibels (0.1f / 0.9f), 0.5);
    }

    // Toggle off: the same hot side chain is ignored.
    {
        auto p = makeBusProcessor (juce::AudioChannelSet::stereo(), true);
        setParam (*p, znth::id::ceiling, -6.0f);
        const float peak = runScSignal (*p, 0.1f, 0.9f);
        expectNear ("ext SC off ignores the side-chain bus (dB)",
                    juce::Decibels::gainToDecibels (peak),
                    juce::Decibels::gainToDecibels (0.1f), 0.2);
    }

    // Stem mastering: two instances at different stem levels but the same
    // trigger must apply the identical gain envelope (outputs scale
    // exactly), natively and with oversampling engaged.
    for (const int osChoice : { 0, 2 })
    {
        auto p1 = makeBusProcessor (juce::AudioChannelSet::stereo(), true);
        auto p2 = makeBusProcessor (juce::AudioChannelSet::stereo(), true);
        for (auto* p : { p1.get(), p2.get() })
        {
            setParam (*p, znth::id::extSidechain, 1.0f);
            setParam (*p, znth::id::ceiling, -6.0f);
            setParam (*p, znth::id::oversampling, (float) osChoice);
        }

        std::vector<float> out1, out2;
        const float peak1 = runScSignal (*p1, 0.2f, 0.9f, &out1);
        runScSignal (*p2, 0.05f, 0.9f, &out2);

        double maxDiff = 0.0;
        for (size_t i = 4096; i < out1.size(); ++i)
            maxDiff = juce::jmax (maxDiff, (double) std::abs (out1[i] * 0.25f - out2[i]));

        char what[96];
        std::snprintf (what, sizeof (what),
                       "stems duck identically off a shared trigger (os %s)",
                       osChoice == 0 ? "off" : "4x");
        expectNear (what, maxDiff, 0.0, osChoice == 0 ? 1.0e-6 : 1.0e-4);
        expectTrue ("shared trigger is actually reducing",
                    juce::Decibels::gainToDecibels (peak1)
                        < juce::Decibels::gainToDecibels (0.2f) - 3.0);
    }

    // A side chain quieter than the mains: the safety clamp still holds the
    // hard ceiling contract on the output.
    {
        auto p = makeBusProcessor (juce::AudioChannelSet::stereo(), true);
        setParam (*p, znth::id::extSidechain, 1.0f);
        setParam (*p, znth::id::ceiling, -6.0f);
        const float peak = runScSignal (*p, 1.2f, 0.05f);
        expectTrue ("quiet trigger + hot main: ceiling still enforced",
                    peak <= juce::Decibels::decibelsToGain (-6.0f) + 1.0e-4f);
    }
}

// BS.1770-4 surround weighting in the loudness suite: a surround channel
// reads ~+1.5 LU over the same signal on a front, the LFE reads silence.
void testSurroundLoudnessWeights()
{
    auto measure = [] (int channel)
    {
        jdm::LoudnessSuite suite;
        suite.prepare (sr, 6);
        const float w[6] = { 1.0f, 1.0f, 1.0f, 0.0f, 1.41f, 1.41f }; // L R C LFE Ls Rs
        suite.setChannelWeights (w, 6);

        std::vector<float> sig (4800), zeros (4800, 0.0f);
        const float* chans[6];
        for (int c = 0; c < 6; ++c)
            chans[c] = c == channel ? sig.data() : zeros.data();

        const double inc = juce::MathConstants<double>::twoPi * 997.0 / sr;
        long n = 0;
        for (int chunk = 0; chunk < 80; ++chunk) // 8 s
        {
            for (int i = 0; i < 4800; ++i, ++n)
                sig[(size_t) i] = 0.5f * (float) std::sin (inc * (double) n);
            suite.process (chans, 4800);
            suite.update();
        }
        suite.computeGatedNow();
        return (double) suite.integratedLufs();
    };

    // A single-channel 997 Hz sine at amp a reads 20 log10(a) - 3.01 LUFS
    // (BS.1770 calibrates a full-scale single channel to -3.01 LKFS).
    const double front = measure (0);
    expectNear ("front channel at 997 Hz calibration (LUFS)",
                front, 20.0 * std::log10 (0.5) - 3.0103, 0.35);
    expectNear ("surround channel reads +1.5 LU (BS.1770 weighting)",
                measure (4) - front, 10.0 * std::log10 (1.41), 0.1);
    expectTrue ("LFE is excluded from loudness", measure (3) <= -99.0);
}

// --- v0.6: MIDI Learn & host-integration hardening --------------------------

// MIDI Learn basics: arm -> capture -> immediate application, persistence
// through commit and a session round-trip, and clearing.
void testMidiLearn()
{
    auto p = makeProcessor();
    juce::AudioBuffer<float> buf (2, blockSize);
    buf.clear();
    juce::MidiBuffer midi;

    // Arm, then send CC 20 at 32: the mapping is captured and applied at
    // once (gain = -30 + 32/127 * 70 dB).
    p->armMidiLearn (znth::id::gain);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 20, 32), 0);
    p->processBlock (buf, midi);
    midi.clear();
    expectNear ("midi learn: captured CC applies immediately",
                p->apvts.getRawParameterValue (znth::id::gain)->load(),
                -30.0 + 32.0 / 127.0 * 70.0, 0.05);

    p->commitPendingMidiLearn();
    expectNear ("midi learn: mapping persisted in state",
                p->getMappedCc (znth::id::gain), 20.0, 0.0);

    // The mapping survives a state round-trip into a fresh processor.
    juce::MemoryBlock state;
    p->getStateInformation (state);

    auto p2 = makeProcessor();
    p2->setStateInformation (state.getData(), (int) state.getSize());
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 20, 127), 0);
    p2->processBlock (buf, midi);
    midi.clear();
    expectNear ("midi learn: mapping works after state reload",
                p2->apvts.getRawParameterValue (znth::id::gain)->load(), 40.0, 0.05);

    // Clearing the mapping stops CC control.
    p2->clearMidiMapping (znth::id::gain);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 20, 0), 0);
    p2->processBlock (buf, midi);
    midi.clear();
    expectNear ("midi learn: cleared mapping is inert",
                p2->apvts.getRawParameterValue (znth::id::gain)->load(), 40.0, 0.05);
}

// Host-integration hardening (feature 23 QA): the CC lookup is a cache of
// the state tree, so every path that reapplies a tree wholesale — undo,
// redo, A/B slot switches, preset loads — must keep it in sync.
void testMidiMapStateOps()
{
    auto p = makeProcessor();
    juce::AudioBuffer<float> buf (2, blockSize);
    buf.clear();
    juce::MidiBuffer midi;

    auto sendCc = [&] (int cc, int value)
    {
        midi.addEvent (juce::MidiMessage::controllerEvent (1, cc, value), 0);
        p->processBlock (buf, midi);
        midi.clear();
    };
    auto ceiling = [&] { return p->apvts.getRawParameterValue (znth::id::ceiling)->load(); };

    // Baseline snapshot, then map CC 21 -> ceiling and commit.
    p->stateManager.commitPendingChanges();
    p->armMidiLearn (znth::id::ceiling);
    sendCc (21, 64);
    p->commitPendingMidiLearn();
    p->stateManager.commitPendingChanges();
    expectTrue ("mapping recorded before undo", p->getMappedCc (znth::id::ceiling) == 21);

    // Undo removes the mapping from the state AND from the live CC cache.
    p->stateManager.undo();
    expectTrue ("undo removes the mapping", p->getMappedCc (znth::id::ceiling) == -1);
    const float undone = ceiling();
    sendCc (21, 127);
    expectNear ("undone mapping is live-inert", ceiling(), undone, 1.0e-4);

    // Redo restores mapping + cache.
    p->stateManager.redo();
    expectTrue ("redo restores the mapping", p->getMappedCc (znth::id::ceiling) == 21);
    sendCc (21, 0);
    expectNear ("redone mapping is live again", ceiling(), -30.0, 1.0e-3);

    // A/B: slot B never had the mapping; the cache must follow each switch.
    p->stateManager.commitPendingChanges();
    p->stateManager.setActiveSlot (1);
    expectTrue ("slot B carries no mapping", p->getMappedCc (znth::id::ceiling) == -1);
    const float slotB = ceiling();
    sendCc (21, 127);
    expectNear ("CC is inert in slot B", ceiling(), slotB, 1.0e-4);

    p->stateManager.setActiveSlot (0);
    expectTrue ("mapping returns with slot A", p->getMappedCc (znth::id::ceiling) == 21);
    sendCc (21, 127);
    expectNear ("CC is live again back in slot A", ceiling(), 0.0, 1.0e-3);

    // Preset loads inherit the session's mappings (a sound never silently
    // unplugs the controller), while the preset's own parameters apply.
    const auto& list = p->presets.getPresets();
    expectTrue ("factory presets scanned", list.size() >= 2);
    p->presets.loadPreset (list[1]); // "Transparent Master"
    expectNear ("preset parameters applied",
                p->apvts.getRawParameterValue (znth::id::release)->load(), 400.0, 1.0e-3);
    expectTrue ("preset load keeps the mapping", p->getMappedCc (znth::id::ceiling) == 21);
}

// Automation accuracy (v0.6 pass over the v0.2 methodology): every
// continuously audible parameter — now including release — takes a new
// random target every block for 200 blocks, with gain hammered through a
// mapped MIDI CC (the exact path a controller uses). Output must stay
// click-free and inside the automated ceiling throughout.
void testAutomationAccuracy()
{
    auto p = makeProcessor();
    juce::Random rng (11);

    juce::AudioBuffer<float> buf (2, blockSize);
    buf.clear();
    juce::MidiBuffer midi;

    p->armMidiLearn (znth::id::gain);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 30, 64), 0);
    p->processBlock (buf, midi);
    midi.clear();
    p->commitPendingMidiLearn();

    std::vector<float> out;
    long n = 0;
    for (int b = 0; b < 200; ++b)
    {
        setParam (*p, znth::id::ceiling, -1.0f - rng.nextFloat() * 5.0f);
        setParam (*p, znth::id::release, 10.0f + rng.nextFloat() * 500.0f);
        setParam (*p, znth::id::linkTransient, rng.nextFloat() * 100.0f);
        setParam (*p, znth::id::linkRelease, rng.nextFloat() * 100.0f);
        midi.addEvent (juce::MidiMessage::controllerEvent (
            1, 30, 77 + rng.nextInt (22)), 0); // gain 12..24 dB via the CC

        for (int i = 0; i < blockSize; ++i, ++n)
        {
            const float v = 0.9f * std::sin (0.02f * (float) n);
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        p->processBlock (buf, midi);
        midi.clear();
        for (int i = 0; i < blockSize; ++i)
            out.push_back (buf.getSample (0, i));
    }

    double maxDelta = 0.0;
    float peak = 0.0f;
    for (size_t i = 4801; i < out.size(); ++i)
    {
        maxDelta = juce::jmax (maxDelta, (double) std::abs (out[i] - out[i - 1]));
        peak = juce::jmax (peak, std::abs (out[i]));
    }

    expectTrue ("dense automation incl. release + CC is click-free (delta < 0.08)",
                maxDelta < 0.08);
    expectTrue ("automated ceiling never exceeded under CC-driven gain",
                peak <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-5f);
}

// v0.4 scope ring: the display's data source advances and carries sane
// frames while limiting.
void testScopeRing()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::gain, 12.0f);

    runSignal (*p, 2, 40, [] (long n) { return 0.9f * std::sin (0.05f * (float) n); });

    const auto frames = p->scopeWriteIndex.load();
    expectTrue ("scope ring advanced (~80 frames for 40 blocks)",
                frames >= 70 && frames <= 90);

    bool sane = true;
    for (uint32_t i = frames - 8; i < frames; ++i)
    {
        const auto& f = p->scopeRing[i & (uint32_t) (ZenithAudioProcessor::scopeSize - 1)];
        sane = sane && f.inPk > 1.0f            // post-gain input is hot
               && f.outPk <= 0.9672f + 1.0e-4f  // limited to the -0.3 ceiling
               && f.grDb > 3.0f;                // and the GR trace shows it
    }
    expectTrue ("recent scope frames carry limited levels + GR", sane);
}

// Preset metadata wrapper round-trip via the shared parser and Zenith's
// product config.
void testPresetRoundTrip()
{
    const auto cfg = znth::makePresetConfig();

    auto p = makeProcessor();
    setParam (*p, znth::id::gain, 7.5f);
    setParam (*p, znth::id::style, (float) znth::Style::modern);

    auto stateXml = p->apvts.copyState().createXml();
    juce::XmlElement root (cfg.presetTag);
    root.setAttribute ("stateVersion", cfg.stateVersion);
    root.setAttribute ("name", "RoundTrip");
    root.addChildElement (stateXml.release());

    auto tree = znth::PresetManager::stateFromPresetXml (root, cfg);
    expectTrue ("preset wrapper unwraps to a valid state", tree.isValid());

    auto p2 = makeProcessor();
    p2->stateManager.applyState (tree);
    expectNear ("gain survives preset round-trip",
                p2->apvts.getRawParameterValue (znth::id::gain)->load(), 7.5, 1.0e-4);
    expectNear ("style survives preset round-trip",
                p2->apvts.getRawParameterValue (znth::id::style)->load(),
                (double) (int) znth::Style::modern, 1.0e-4);

    juce::XmlElement future (cfg.presetTag);
    future.setAttribute ("stateVersion", cfg.stateVersion + 1);
    expectTrue ("newer-schema preset rejected whole",
                ! znth::PresetManager::stateFromPresetXml (future, cfg).isValid());
}

// v0.7 preset ecosystem QA: the factory library covers every style, and the
// import/export path round-trips cleanly while rejecting the edge cases
// (malformed files, another product's presets, collisions).
void testPresetEcosystem()
{
    auto p = makeProcessor();
    auto& pm = p->presets;
    const auto cfg = znth::makePresetConfig();

    // Every limiting style is represented across the factory library.
    std::array<bool, (size_t) znth::Style::numStyles> styleSeen {};
    znth::PresetInfo streamingSafe;
    int factoryCount = 0;
    for (const auto& info : pm.getPresets())
    {
        if (! info.factory)
            continue;
        ++factoryCount;
        if (info.name == "Streaming Safe")
            streamingSafe = info;
        pm.loadPreset (info);
        const int s = (int) std::lround (
            p->apvts.getRawParameterValue (znth::id::style)->load());
        if (s >= 0 && s < (int) styleSeen.size())
            styleSeen[(size_t) s] = true;
    }
    expectTrue ("factory library expanded to 16+ presets", factoryCount >= 16);
    bool allStyles = true;
    for (bool seen : styleSeen)
        allStyles = allStyles && seen;
    expectTrue ("every limiting style has a factory preset", allStyles);

    auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("zenith_preset_qa");
    tempDir.deleteRecursively();
    tempDir.createDirectory();

    // Export a factory preset to a uniquely named file, then re-parse it: the
    // .znth on-disk format round-trips.
    const juce::String stem = "ZenithQA_"
        + juce::String (juce::Random::getSystemRandom().nextInt (1000000));
    juce::File src = tempDir.getChildFile (stem);
    const bool exported = pm.exportPreset (streamingSafe, src);
    src = src.withFileExtension (cfg.extension);
    expectTrue ("factory preset exports to a .znth file",
                exported && src.existsAsFile());
    if (auto xml = juce::parseXML (src))
        expectTrue ("exported preset re-parses to a valid state",
                    znth::PresetManager::stateFromPresetXml (*xml, cfg).isValid());
    else
        expectTrue ("exported preset re-parses to a valid state", false);

    // Import it twice: the second import must not overwrite the first.
    const int before = (int) pm.getPresets().size();
    const bool imp1 = pm.importPreset (src);
    const bool imp2 = pm.importPreset (src);
    expectTrue ("valid .znth imports", imp1 && imp2);
    expectTrue ("re-import is collision-safe (two distinct user presets)",
                (int) pm.getPresets().size() == before + 2);

    // Malformed and wrong-product files are rejected, not half-loaded.
    auto junk = tempDir.getChildFile ("junk.znth");
    junk.replaceWithText ("<not-a-preset><nope/></not-a-preset>");
    expectTrue ("malformed file rejected on import", ! pm.importPreset (junk));

    auto wrong = tempDir.getChildFile ("wrong.znth");
    {
        juce::XmlElement root ("MeridianPreset"); // another product's tag
        root.setAttribute ("stateVersion", 1);
        root.addChildElement (new juce::XmlElement ("MeridianState"));
        wrong.replaceWithText (root.toString());
    }
    expectTrue ("another product's preset rejected on import",
                ! pm.importPreset (wrong));

    // Cleanup: remove the user presets this test created and the temp dir.
    for (const auto& info : pm.getPresets())
        if (! info.factory && info.file.getFileNameWithoutExtension().startsWith (stem))
            pm.deleteUserPreset (info);
    tempDir.deleteRecursively();
}

// A/B slots: edits in A stay out of B, and each slot's undo is independent.
void testStateManagerSlots()
{
    auto p = makeProcessor();
    auto& sm = p->stateManager;

    setParam (*p, znth::id::gain, 5.0f);
    sm.commitPendingChanges();

    sm.setActiveSlot (1);
    expectNear ("slot B starts from the construction-time default (gain)",
                p->apvts.getRawParameterValue (znth::id::gain)->load(), 0.0, 1.0e-4);
    setParam (*p, znth::id::gain, -10.0f);
    sm.commitPendingChanges();

    sm.setActiveSlot (0);
    expectNear ("slot A keeps its own gain",
                p->apvts.getRawParameterValue (znth::id::gain)->load(), 5.0, 1.0e-4);

    sm.setActiveSlot (1);
    expectNear ("slot B keeps its own gain",
                p->apvts.getRawParameterValue (znth::id::gain)->load(), -10.0, 1.0e-4);

    sm.undo();
    expectTrue ("undo in B restores B's previous state",
                std::abs (p->apvts.getRawParameterValue (znth::id::gain)->load() - (-10.0f)) > 1.0f);

    sm.setActiveSlot (0);
    expectNear ("B's undo did not touch A",
                p->apvts.getRawParameterValue (znth::id::gain)->load(), 5.0, 1.0e-4);
}

// Session save/load round-trip through the binary state blob.
void testSessionRoundTrip()
{
    auto p = makeProcessor();
    setParam (*p, znth::id::ceiling, -2.0f);
    setParam (*p, znth::id::release, 500.0f);
    p->stateManager.commitPendingChanges();

    juce::MemoryBlock blob;
    p->getStateInformation (blob);

    auto p2 = makeProcessor();
    p2->setStateInformation (blob.getData(), (int) blob.getSize());

    expectNear ("ceiling survives session round-trip",
                p2->apvts.getRawParameterValue (znth::id::ceiling)->load(), -2.0, 1.0e-4);
    expectNear ("release survives session round-trip",
                p2->apvts.getRawParameterValue (znth::id::release)->load(), 500.0, 1.0e-3);
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("Zenith DSP verification (sr %.0f, block %d)\n\n", sr, blockSize);

    testBelowCeilingNull();
    testCeilingEnforcement();
    testSteadyStateGainReduction();
    testLatencyReporting();
    testReleaseRecovery();
    testChannelLinking();
    testDcFilter();
    testUnityGain();
    testDeltaAudition();
    testBypassNull();
    testMonoPath();
    testStyleCharacters();
    testParameterInterpolation();
    testOversampling();
    testTruePeak();
    testDither();
    testLoudnessSuite();
    testSurroundLayouts();
    testSurroundLinkTopology();
    testExternalSidechain();
    testSurroundLoudnessWeights();
    testMidiLearn();
    testMidiMapStateOps();
    testAutomationAccuracy();
    testScopeRing();
    benchmarkStylesAndFactors();
    testPresetRoundTrip();
    testPresetEcosystem();
    testStateManagerSlots();
    testSessionRoundTrip();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                 failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
