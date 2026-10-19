#pragma once

#include <juce_dsp/juce_dsp.h>
#include "../Parameters.h"

namespace znth
{

// Lookahead brickwall limiter core.
//
// Per sample, per channel:
//
//   1. instantaneous peak |x|, blended toward the loudest channel by the
//      transient link amount;
//   2. required gain r = min (1, ceiling / peak);
//   3. sliding-window minimum of r over the last L+1 samples (L = lookahead)
//      — the instant-attack envelope;
//   4. styled release: dual-stage (fast + slow) exponential recovery as a
//      rise-rate limit toward 1, optionally frozen for a per-style transient
//      hold, programme-dependent when Auto Release (or the style) asks for
//      it, and blended across channels by the release link amount;
//   5. attack smoothing: two cascaded box-car means with total support
//      A <= L, which turns the staircase into a smooth ramp *without ever
//      rising above the required gain*.
//
// The audio is delayed by exactly L samples. No-overshoot guarantee: the
// smoothed envelope e[n] is a convex combination of release values at
// k in [n-A+1, n]; each of those is <= the sliding minimum m[k], whose
// window [k-L, k] contains n-L whenever A <= L — so e[n] <= r[n-L], the
// required gain of the sample being output. A safety clamp at the ceiling
// exists after that but the tests prove it never engages at a static
// ceiling. (While the *ceiling itself* ramps down — v0.2 smart parameter
// interpolation smooths it over ~20 ms — the envelope can lag the new
// target by up to one lookahead and the clamp bridges that gap; output
// still never exceeds the instantaneous smoothed ceiling.)
//
// Styles (v0.2): trait-table ballistics giving each algorithm a distinct,
// regression-tested character — attack-window scale, release scale,
// dual-stage fast-recovery depth/speed, transient hold, link floors, and
// an extra ceiling margin for Safe.
//
// v0.5 — routing breadth:
//   * up to 8 channels (5.1/7.1). The link topology treats all full-range
//     channels as one group (transient link blends toward the group's
//     loudest, release link toward its most-reduced); the LFE channel is
//     excluded in both directions — it neither ducks the mains nor gets
//     ducked by them — but is still independently limited to the ceiling.
//     Mono/stereo behaviour is unchanged (no LFE, group = all channels).
//   * an optional external detector (side chain): peaks are taken from the
//     detector signal while the gain is applied to the main audio, so
//     grouped stem instances fed the same side chain duck identically. The
//     no-overshoot proof then holds for the *detector*; the safety clamp
//     still hard-enforces the ceiling on the main output (it can engage
//     when the mains run hotter than the side chain — documented, and the
//     stem-mastering use case keeps stems below their summed trigger).
class LimiterEngine
{
public:
    static constexpr int maxChannels = 8;
    struct Params
    {
        float ceilingDb = -0.3f;
        float releaseMs = 100.0f;
        bool autoRelease = false;
        float attackMs = 2.0f;
        float lookaheadMs = 2.0f;
        Style style = Style::transparent;
        float linkTransient01 = 1.0f;
        float linkRelease01 = 1.0f;
    };

    struct StyleTraits
    {
        float attackScale;      // scales the smoothing window (clamped to lookahead)
        float releaseScale;     // scales the release time
        float marginDb;         // extra headroom below the ceiling
        float minLink;          // floor on both link amounts (1 = channels always together)
        bool programmeRelease;  // release adapts to sustained GR regardless of autoRelease
        float fastRatio;        // fast stage release time = release * fastRatio (0 = single-stage)
        float fastDepth;        // fraction of the recovery carried by the fast stage
        float holdMs;           // transient hold before the release starts
    };

    static const StyleTraits& styleTraits (Style s)
    {
        static const std::array<StyleTraits, (size_t) Style::numStyles> table { {
            //                    atk    rel  margin link  prog   fast   depth  hold
            /* Transparent */ { 1.0f,  1.0f, 0.0f,  0.0f, false, 0.0f,  0.0f,  0.0f },
            /* Punch       */ { 0.35f, 0.7f, 0.0f,  0.0f, false, 0.12f, 0.6f,  0.0f },
            /* Dynamic     */ { 0.8f,  1.0f, 0.0f,  0.0f, true,  0.2f,  0.4f,  0.0f },
            /* Versatile   */ { 1.25f, 1.1f, 0.0f,  0.0f, false, 0.25f, 0.3f,  1.0f },
            /* Aggressive  */ { 0.25f, 0.4f, 0.0f,  0.0f, false, 0.1f,  0.7f,  0.0f },
            /* Modern      */ { 1.2f,  1.3f, 0.0f,  0.0f, true,  0.18f, 0.5f,  3.0f },
            /* Bus         */ { 1.5f,  2.0f, 0.0f,  1.0f, false, 0.0f,  0.0f,  5.0f },
            /* Safe        */ { 4.0f,  1.4f, 0.1f,  1.0f, false, 0.0f,  0.0f,  2.0f },
        } };
        return table[(size_t) juce::jlimit (0, (int) Style::numStyles - 1, (int) s)];
    }

    // lfeChannel: index of the LFE channel in the layout (-1 = none). It is
    // limited independently and excluded from the link group.
    void prepare (double sampleRate, int numChannels, int lfeChannel = -1)
    {
        fs = sampleRate;
        channels = juce::jlimit (1, maxChannels, numChannels);
        lfeIndex = lfeChannel >= 0 && lfeChannel < channels ? lfeChannel : -1;

        const int maxLookahead = msToSamples (maxLookaheadMs) + 1;
        for (int c = 0; c < channels; ++c)
        {
            auto& ch = state[(size_t) c];
            ch.delay.assign ((size_t) maxLookahead + 1, 0.0f);
            ch.minDeque.assign ((size_t) maxLookahead + 2, { 1.0f, 0 });
            ch.box1.assign ((size_t) maxLookahead + 1, 1.0f);
            ch.box2.assign ((size_t) maxLookahead + 1, 1.0f);
        }

        // Smart parameter interpolation: ceiling and link amounts ramp
        // per-sample over ~20 ms instead of stepping at block boundaries.
        for (auto* s : { &ceilingSmooth, &linkTSmooth, &linkRSmooth })
            s->reset (sampleRate, 0.02);

        configured = false;
        applyParams();

        ceilingSmooth.setCurrentAndTargetValue (ceilingSmooth.getTargetValue());
        linkTSmooth.setCurrentAndTargetValue (linkTSmooth.getTargetValue());
        linkRSmooth.setCurrentAndTargetValue (linkRSmooth.getTargetValue());
        reset();
    }

    void setParams (const Params& p)
    {
        params = p;
        applyParams();
    }

    int getLookaheadSamples() const { return lookaheadSamples; }

    // Two-channel convenience wrapper (internal detection) — the historic
    // signature, used by the stereo tests.
    float process (float* l, float* r, int n,
                   float* dryOutL = nullptr, float* dryOutR = nullptr) noexcept
    {
        float* audio[2] = { l, r };
        float* dry[2] = { dryOutL, dryOutR };
        return process (audio, r != nullptr ? 2 : 1, n, nullptr,
                        dryOutL != nullptr || dryOutR != nullptr ? dry : nullptr);
    }

    // Limits in place. detector (optional) supplies the side-chain signal
    // the gain computation reads instead of the audio — one pointer per
    // audio channel (the caller maps a narrower side chain onto the main
    // layout). dryOut (optional, per-channel, entries may be null) receives
    // the lookahead-delayed input — the exact signal the limiter is judged
    // against, for the delta audition path. Returns the maximum gain
    // reduction in dB this block (positive = reducing).
    float process (float* const* audio, int numChIn, int n,
                   const float* const* detector = nullptr,
                   float* const* dryOut = nullptr) noexcept
    {
        const int numCh = juce::jlimit (1, channels, numChIn);
        float minGain = 1.0f;

        for (int i = 0; i < n; ++i)
        {
            const float ceiling = ceilingSmooth.getNextValue();
            const float tl = linkTSmooth.getNextValue();
            const float lr = linkRSmooth.getNextValue();

            // 1. peaks + transient link. The LFE keeps its own detection —
            // it neither raises the group's peak nor follows it.
            float peak[maxChannels];
            float groupMax = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                peak[c] = std::abs ((detector != nullptr ? detector[c] : audio[c])[i]);
                if (c != lfeIndex)
                    groupMax = juce::jmax (groupMax, peak[c]);
            }

            float rel[maxChannels];
            float groupRelMin = 1.0f;
            for (int c = 0; c < numCh; ++c)
            {
                auto& s = state[(size_t) c];

                const float linked = c == lfeIndex
                    ? peak[c]
                    : peak[c] + tl * (groupMax - peak[c]);
                const float required = linked > ceiling ? ceiling / linked : 1.0f;

                // 2. sliding minimum over the last window samples.
                pushMin (s, required);
                const float m = s.minDeque[(size_t) s.dqHead].value;

                // 3. styled release. A new deeper reduction re-arms the
                // transient hold; both stages snap down instantly and only
                // rise once the hold has run out.
                if (m < s.combined)
                    s.holdCount = holdSamples;

                const float scale = releaseScaleFor (s, m);
                const bool held = s.holdCount > 0;
                if (held)
                    --s.holdCount;

                stageStep (s.relSlow, m, held, releaseCoeffSlow * scale);
                if (fastDepth > 0.0f)
                    stageStep (s.relFast, m, held, releaseCoeffFast * scale);

                s.combined = fastDepth > 0.0f
                    ? juce::jmin (m, fastDepth * s.relFast + (1.0f - fastDepth) * s.relSlow)
                    : s.relSlow;
                rel[c] = s.combined;
                if (c != lfeIndex)
                    groupRelMin = juce::jmin (groupRelMin, rel[c]);
            }

            // 4. release link: blend the group toward its most-reduced
            // channel (the LFE keeps its own envelope).
            if (numCh > 1)
                for (int c = 0; c < numCh; ++c)
                    if (c != lfeIndex)
                        rel[c] += lr * (groupRelMin - rel[c]);

            for (int c = 0; c < numCh; ++c)
            {
                auto& s = state[(size_t) c];

                // 5. cascaded box-car attack smoothing.
                const float e = boxPush (s.box2, s.b2Pos, s.b2Sum, boxLen2,
                          boxPush (s.box1, s.b1Pos, s.b1Sum, boxLen1, rel[c]));
                minGain = juce::jmin (minGain, e);

                // Ring of exactly L slots: read-before-write at one index is
                // an L-sample delay, matching the reported latency and the
                // n-L index in the overshoot proof.
                float* data = audio[c];
                const float delayed = s.delay[(size_t) s.dPos];
                s.delay[(size_t) s.dPos] = data[i];
                s.dPos = s.dPos + 1 >= lookaheadSamples ? 0 : s.dPos + 1;

                if (dryOut != nullptr && dryOut[c] != nullptr)
                    dryOut[c][i] = delayed;

                data[i] = juce::jlimit (-ceiling, ceiling, delayed * e);
            }
        }

        return juce::Decibels::gainToDecibels (juce::jmax (minGain, 1.0e-6f), -120.0f) * -1.0f;
    }

    void reset()
    {
        for (auto& s : state)
        {
            std::fill (s.delay.begin(), s.delay.end(), 0.0f);
            std::fill (s.box1.begin(), s.box1.end(), 1.0f);
            std::fill (s.box2.begin(), s.box2.end(), 1.0f);
            s.b1Sum = (double) boxLen1;
            s.b2Sum = (double) boxLen2;
            s.b1Pos = s.b2Pos = 0;
            s.dPos = 0;
            s.dqHead = s.dqTail = 0;
            s.dqCount = 0;
            s.sampleIndex = 0;
            s.relSlow = s.relFast = s.combined = 1.0f;
            s.holdCount = 0;
            s.sustainedGr = 0.0f;
        }
    }

private:
    int msToSamples (float ms) const
    {
        return juce::jmax (1, (int) std::lround (fs * (double) ms * 0.001));
    }

    // Re-derives sample counts; only rebuilds (and resets) the windowed
    // structures when a count actually changed, so per-block calls with an
    // unchanged setup cost nothing and cause no discontinuity. Attack and
    // lookahead changes are structural and reset the envelope; everything
    // else ramps smoothly.
    void applyParams()
    {
        traits = &styleTraits (params.style);

        const int newLookahead = msToSamples (juce::jlimit (minLookaheadMs, maxLookaheadMs,
                                                            params.lookaheadMs));
        const int attackSamples = juce::jlimit (1, newLookahead,
            msToSamples (params.attackMs * traits->attackScale));
        const int newBox1 = juce::jmax (1, (attackSamples + 1) / 2);
        const int newBox2 = juce::jmax (1, attackSamples - newBox1 + 1);

        if (! configured || newLookahead != lookaheadSamples
            || newBox1 != boxLen1 || newBox2 != boxLen2)
        {
            lookaheadSamples = newLookahead;
            boxLen1 = newBox1;
            boxLen2 = newBox2;
            configured = true;
            reset();
        }

        ceilingSmooth.setTargetValue (juce::Decibels::decibelsToGain (
            juce::jlimit (minCeilingDb, maxCeilingDb, params.ceilingDb) - traits->marginDb));
        linkTSmooth.setTargetValue (juce::jmax (params.linkTransient01, traits->minLink));
        linkRSmooth.setTargetValue (juce::jmax (params.linkRelease01, traits->minLink));

        const double relMs = juce::jlimit (minReleaseMs, maxReleaseMs,
                                           params.releaseMs * traits->releaseScale);
        releaseCoeffSlow = 1.0f - (float) std::exp (-1.0 / (fs * relMs * 0.001));

        // Dual-stage recovery: the style's own shape, or a default fast
        // stage when Auto Release is engaged on a single-stage style.
        float ratio = traits->fastRatio;
        fastDepth = traits->fastDepth;
        if (params.autoRelease && fastDepth <= 0.0f)
        {
            ratio = 0.15f;
            fastDepth = 0.4f;
        }
        releaseCoeffFast = ratio > 0.0f
            ? 1.0f - (float) std::exp (-1.0 / (fs * relMs * (double) ratio * 0.001))
            : 1.0f;

        holdSamples = traits->holdMs > 0.0f ? msToSamples (traits->holdMs) : 0;
        sustainCoeff = 1.0f - (float) std::exp (-1.0 / (fs * 0.2)); // 200 ms GR tracker
    }

    struct ChannelState
    {
        std::vector<float> delay;
        int dPos = 0;

        struct MinEntry { float value; juce::int64 expires; };
        std::vector<MinEntry> minDeque;   // monotonic increasing values, ring
        int dqHead = 0, dqTail = 0, dqCount = 0;
        juce::int64 sampleIndex = 0;      // 64-bit: never wraps mid-session

        std::vector<float> box1, box2;
        int b1Pos = 0, b2Pos = 0;
        double b1Sum = 0.0, b2Sum = 0.0;

        float relSlow = 1.0f, relFast = 1.0f, combined = 1.0f;
        int holdCount = 0;
        float sustainedGr = 0.0f; // smoothed (1 - gain), for programme release
    };

    void pushMin (ChannelState& s, float value) noexcept
    {
        const int cap = (int) s.minDeque.size();
        // Drop entries dominated by the new value (they can never be the min
        // again while this one is in the window).
        while (s.dqCount > 0)
        {
            const int back = (s.dqTail + cap - 1) % cap;
            if (s.minDeque[(size_t) back].value < value)
                break;
            s.dqTail = back;
            --s.dqCount;
        }
        s.minDeque[(size_t) s.dqTail] = { value, s.sampleIndex + lookaheadSamples + 1 };
        s.dqTail = (s.dqTail + 1) % cap;
        ++s.dqCount;

        ++s.sampleIndex;
        // Expire the front once it leaves the L+1 window.
        if (s.minDeque[(size_t) s.dqHead].expires <= s.sampleIndex)
        {
            s.dqHead = (s.dqHead + 1) % cap;
            --s.dqCount;
        }
    }

    // One release stage: snap down with the minimum, rise toward 1 at the
    // stage rate once the hold has expired.
    static void stageStep (float& stage, float m, bool held, float k) noexcept
    {
        if (m < stage)
            stage = m;
        else if (! held)
            stage += (1.0f - stage) * juce::jmin (1.0f, k);
    }

    static float boxPush (std::vector<float>& buf, int& pos, double& sum,
                          int len, float value) noexcept
    {
        sum += (double) value - (double) buf[(size_t) pos];
        buf[(size_t) pos] = value;
        pos = pos + 1 >= len ? 0 : pos + 1;
        return (float) (sum / (double) len);
    }

    float releaseScaleFor (ChannelState& s, float m) noexcept
    {
        const float gr = 1.0f - m;
        s.sustainedGr += sustainCoeff * (gr - s.sustainedGr);

        if (! params.autoRelease && ! traits->programmeRelease)
            return 1.0f;

        // Programme-dependent release: sustained reduction slows the
        // recovery up to 2x; brief peaks recover up to ~3x faster.
        const float sustain01 = juce::jlimit (0.0f, 1.0f, s.sustainedGr * 4.0f);
        return juce::jmap (sustain01, 3.0f, 0.5f);
    }

    Params params;
    const StyleTraits* traits = &styleTraits (Style::transparent);

    double fs = 48000.0;
    int channels = 2;
    int lfeIndex = -1;
    bool configured = false;

    int lookaheadSamples = 96;
    int boxLen1 = 48, boxLen2 = 48;
    float releaseCoeffSlow = 0.001f;
    float releaseCoeffFast = 1.0f;
    float fastDepth = 0.0f;
    int holdSamples = 0;
    float sustainCoeff = 0.001f;

    juce::SmoothedValue<float> ceilingSmooth, linkTSmooth, linkRSmooth;

    std::array<ChannelState, maxChannels> state;
};

} // namespace znth
