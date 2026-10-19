#pragma once

#include <juce_core/juce_core.h>
#include "../Parameters.h"

namespace znth
{

// Output dither (v0.3, feature 15): TPDF dither at 16- or 24-bit depth with
// selectable noise shaping of the quantization error.
//
// Error-feedback topology: w[n] = x[n] - sum_k c_k * e[n-k], the TPDF
// dither (+-1 LSB triangular) is added to w before quantizing to the target
// grid, and e[n] = y[n] - w[n] is fed back. Shapers:
//
//   Flat        - no feedback: plain TPDF dither
//   Slope       - c = {2, -1}: (1 - z^-1)^2, a simple rising slope
//   F-Weighted  - Lipshitz 5-tap psychoacoustic weighting
//   E-Weighted  - Wannamaker 9-tap E-weighting
//
// Quantized output stays exactly on the target grid (1/2^(bits-1)), which
// the tests verify, along with the shaped error spectrum moving energy out
// of the low band.
class DitherStage
{
public:
    static constexpr int maxChannels = 8;

    void prepare (int numChannels)
    {
        channels = juce::jlimit (1, maxChannels, numChannels);
        reset();
    }

    void reset()
    {
        for (auto& h : history)
            h.fill (0.0f);
    }

    void setParams (int bits, NoiseShaping shaping)
    {
        if (bits != bitDepth || shaping != currentShaping)
            reset();
        bitDepth = bits;          // 0 = off
        currentShaping = shaping;
    }

    // Stereo convenience wrapper — the historic signature. Only valid when
    // prepared with <= 2 channels.
    void process (float* l, float* r, int n) noexcept
    {
        float* chans[2] = { l, r };
        process (chans, n);
    }

    void process (float* const* chans, int n) noexcept
    {
        if (bitDepth <= 0)
            return;

        const float q = 1.0f / (float) (1 << (bitDepth - 1)); // 1 LSB at +-1.0 full scale
        const auto& c = coeffsFor (currentShaping);

        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = chans[ch];
            if (data == nullptr)
                continue;
            auto& e = history[(size_t) ch];

            for (int i = 0; i < n; ++i)
            {
                float w = data[i];
                for (size_t k = 0; k < c.size(); ++k)
                    w -= c[k] * e[k];

                const float dither = (rng.nextFloat() - rng.nextFloat()) * q;
                const float y = q * std::round ((w + dither) / q);

                for (size_t k = e.size() - 1; k > 0; --k)
                    e[k] = e[k - 1];
                e[0] = y - w;

                data[i] = y;
            }
        }
    }

private:
    static const std::vector<float>& coeffsFor (NoiseShaping s)
    {
        static const std::vector<float> flat {};
        static const std::vector<float> slope { 2.0f, -1.0f };
        static const std::vector<float> fWeighted {
            2.033f, -2.165f, 1.959f, -1.590f, 0.6149f };
        static const std::vector<float> eWeighted {
            2.412f, -3.370f, 3.937f, -4.174f, 3.353f,
            -2.205f, 1.281f, -0.569f, 0.0847f };

        switch (s)
        {
            case NoiseShaping::slope:     return slope;
            case NoiseShaping::fWeighted: return fWeighted;
            case NoiseShaping::eWeighted: return eWeighted;
            case NoiseShaping::flat:
            default:                      return flat;
        }
    }

    int channels = 2;
    int bitDepth = 0;
    NoiseShaping currentShaping = NoiseShaping::flat;
    std::array<std::array<float, 9>, (size_t) maxChannels> history {};
    juce::Random rng;
};

} // namespace znth
