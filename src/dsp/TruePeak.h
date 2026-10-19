#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cmath>

namespace znth
{

// Inter-sample (true) peak estimator in the spirit of ITU-R BS.1770-4
// Annex 2: a 4x polyphase FIR interpolator (windowed-sinc prototype, 12
// taps per phase) whose interpolated magnitudes are max-tracked alongside
// the raw sample peaks. Accurate to a few hundredths of a dB — plenty for
// metering and for the v0.3 regression tests; the meter-strip integration
// happens at v0.4.
class TruePeakMeter
{
public:
    static constexpr int phases = 4;
    static constexpr int tapsPerPhase = 12;

    TruePeakMeter()
    {
        // Windowed-sinc interpolation prototype, cutoff at the input
        // Nyquist, Blackman window, centre at (N-1)/2.
        constexpr int n = phases * tapsPerPhase;
        const double centre = (n - 1) * 0.5;
        for (int m = 0; m < n; ++m)
        {
            const double t = ((double) m - centre) / (double) phases;
            const double sinc = t == 0.0 ? 1.0
                                         : std::sin (juce::MathConstants<double>::pi * t)
                                               / (juce::MathConstants<double>::pi * t);
            const double w = 0.42
                - 0.5 * std::cos (juce::MathConstants<double>::twoPi * m / (n - 1))
                + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * m / (n - 1));
            proto[(size_t) m] = (float) (sinc * w);
        }

        // Normalize each phase to unity DC gain so a full-scale DC input
        // reads 0 dBTP exactly.
        for (int p = 0; p < phases; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < tapsPerPhase; ++k)
                sum += proto[(size_t) (k * phases + p)];
            for (int k = 0; k < tapsPerPhase; ++k)
                proto[(size_t) (k * phases + p)] /= (float) sum;
        }
    }

    static constexpr int maxChannels = 8;

    void prepare (int numChannels)
    {
        channels = juce::jlimit (1, maxChannels, numChannels);
        for (auto& h : history)
            h.fill (0.0f);
        maxPeak.store (0.0f);
    }

    // Stereo convenience wrapper — the historic signature. Only valid when
    // prepared with <= 2 channels.
    void process (const float* l, const float* r, int n) noexcept
    {
        const float* chans[2] = { l, r };
        process (chans, n);
    }

    void process (const float* const* chans, int n) noexcept
    {
        float localMax = 0.0f;

        for (int ch = 0; ch < channels; ++ch)
        {
            const float* data = chans[ch];
            if (data == nullptr)
                continue;
            auto& h = history[(size_t) ch];

            for (int i = 0; i < n; ++i)
            {
                for (size_t k = h.size() - 1; k > 0; --k)
                    h[k] = h[k - 1];
                h[0] = data[i];
                localMax = juce::jmax (localMax, std::abs (data[i]));

                for (int p = 0; p < phases; ++p)
                {
                    float acc = 0.0f;
                    for (int k = 0; k < tapsPerPhase; ++k)
                        acc += proto[(size_t) (k * phases + p)] * h[(size_t) k];
                    localMax = juce::jmax (localMax, std::abs (acc));
                }
            }
        }

        float prev = maxPeak.load();
        while (localMax > prev && ! maxPeak.compare_exchange_weak (prev, localMax)) {}
    }

    // Peak-hold since the last read (the GUI/test reads with exchange).
    float readAndReset() { return maxPeak.exchange (0.0f); }
    float read() const { return maxPeak.load(); }

private:
    std::array<float, (size_t) (phases * tapsPerPhase)> proto {};
    std::array<std::array<float, (size_t) tapsPerPhase>, (size_t) maxChannels> history {};
    std::atomic<float> maxPeak { 0.0f };
    int channels = 2;
};

} // namespace znth
