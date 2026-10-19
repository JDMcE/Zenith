#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "LookAndFeel.h"

namespace znth
{

// The real-time level display (v0.4, feature 4): scrolling input/output
// history with the applied gain reduction traced from the top in amber and
// peak-GR labels on recent maxima, plus two further display modes:
//
//   SCROLL - full-range (-60..0 dB) level history
//   ZOOM   - the ceiling strip: output detail from ceiling-9 dB to +3 dB
//   HISTO  - infinite-persistence histogram of gain-reduction depth, with
//            the share of time spent limiting (click the display to reset)
//
// Fed by the processor's lock-free scope ring (256-sample cells, ~5.3 ms
// per column at 48 kHz); repaints from its own 30 Hz timer.
class MainDisplay : public juce::Component,
                    private juce::Timer
{
public:
    explicit MainDisplay (ZenithAudioProcessor& p) : processor (p)
    {
        history.resize (columns, {});

        auto initModeButton = [this] (juce::TextButton& b, const char* text, int mode)
        {
            b.setButtonText (text);
            b.setClickingTogglesState (false);
            b.onClick = [this, mode]
            {
                displayMode = mode;
                repaint();
            };
            addAndMakeVisible (b);
        };
        initModeButton (scrollButton, "SCROLL", 0);
        initModeButton (zoomButton, "ZOOM", 1);
        initModeButton (histoButton, "HISTO", 2);

        scrollButton.setTooltip ("Scrolling level history with the gain reduction "
                                 "traced from the top.");
        zoomButton.setTooltip ("Zoomed ceiling strip: output detail around the ceiling.");
        histoButton.setTooltip ("Infinite-persistence histogram of gain-reduction depth "
                                "(click the display to reset).");

        startTimerHz (30);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 4);
        auto row = r.removeFromTop (18);
        histoButton.setBounds (row.removeFromRight (52));
        row.removeFromRight (4);
        zoomButton.setBounds (row.removeFromRight (44));
        row.removeFromRight (4);
        scrollButton.setBounds (row.removeFromRight (56));
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (displayMode == 2)
        {
            histogram.fill (0.0);
            totalFrames = limitedFrames = 0;
            repaint();
        }
    }

private:
    static constexpr int columns = 480;

    void timerCallback()
    {
        const auto w = processor.scopeWriteIndex.load (std::memory_order_acquire);
        bool fresh = false;
        while (readIndex != w)
        {
            const auto& frame = processor.scopeRing[readIndex
                & (uint32_t) (ZenithAudioProcessor::scopeSize - 1)];
            history[(size_t) historyPos] = frame;
            historyPos = (historyPos + 1) % columns;

            ++totalFrames;
            if (frame.grDb > 0.05f)
            {
                ++limitedFrames;
                const int bin = juce::jlimit (0, numBins - 1, (int) (frame.grDb * 2.0f));
                histogram[(size_t) bin] += 1.0;
            }
            ++readIndex;
            fresh = true;
        }
        if (fresh)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (colors::panel);
        g.fillRoundedRectangle (area, 5.0f);

        auto plot = getLocalBounds().reduced (8, 6).withTrimmedTop (16).toFloat();

        if (displayMode == 2)
            paintHistogram (g, plot);
        else
            paintScroll (g, plot, displayMode == 1);

        g.setColour (colors::outline);
        g.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 1.0f);

        // Active mode marker.
        auto* active = displayMode == 0 ? &scrollButton
                     : displayMode == 1 ? &zoomButton : &histoButton;
        g.setColour (colors::accent);
        g.fillRect (active->getBounds().toFloat().removeFromBottom (2.0f).reduced (4.0f, 0.0f));
    }

    void paintScroll (juce::Graphics& g, juce::Rectangle<float> plot, bool zoom)
    {
        const float ceilingDb = processor.apvts.getRawParameterValue (id::ceiling)->load();
        const float top = zoom ? ceilingDb + 3.0f : 0.0f;
        const float bottom = zoom ? ceilingDb - 9.0f : -60.0f;

        auto yFor = [&] (float db)
        {
            return plot.getY() + (top - juce::jlimit (bottom, top, db))
                                     / (top - bottom) * plot.getHeight();
        };
        auto xFor = [&] (int col)
        {
            return plot.getX() + (float) col / (float) (columns - 1) * plot.getWidth();
        };
        auto frameAt = [this] (int col) -> const ZenithAudioProcessor::ScopeFrame&
        {
            return history[(size_t) ((historyPos + col) % columns)];
        };

        // Grid lines every 12 dB (3 dB zoomed).
        g.setColour (colors::outline.withAlpha (0.5f));
        const float step = zoom ? 3.0f : 12.0f;
        for (float db = std::ceil (bottom / step) * step; db <= top; db += step)
            g.drawHorizontalLine ((int) yFor (db), plot.getX(), plot.getRight());

        // Input trace (dim outline).
        juce::Path inPath;
        for (int c = 0; c < columns; ++c)
        {
            const float y = yFor (juce::Decibels::gainToDecibels (frameAt (c).inPk, bottom));
            if (c == 0) inPath.startNewSubPath (xFor (c), y);
            else        inPath.lineTo (xFor (c), y);
        }
        g.setColour (colors::textDim.withAlpha (0.55f));
        g.strokePath (inPath, juce::PathStrokeType (1.0f));

        // Output as a filled area in the level hue.
        juce::Path outPath;
        outPath.startNewSubPath (xFor (0), plot.getBottom());
        for (int c = 0; c < columns; ++c)
            outPath.lineTo (xFor (c),
                            yFor (juce::Decibels::gainToDecibels (frameAt (c).outPk, bottom)));
        outPath.lineTo (plot.getRight(), plot.getBottom());
        outPath.closeSubPath();
        g.setColour (colors::level.withAlpha (0.30f));
        g.fillPath (outPath);
        g.setColour (colors::level.withAlpha (0.9f));
        g.strokePath (outPath, juce::PathStrokeType (1.0f));

        // Ceiling line in the accent.
        g.setColour (colors::accent.withAlpha (0.9f));
        const float ceilY = yFor (ceilingDb);
        g.drawLine (plot.getX(), ceilY, plot.getRight(), ceilY, 1.2f);

        // Gain reduction from the top, in amber. 24 dB spans 45 % of the plot.
        const float grScale = plot.getHeight() * 0.45f / 24.0f;
        juce::Path grPath;
        grPath.startNewSubPath (xFor (0), plot.getY());
        for (int c = 0; c < columns; ++c)
            grPath.lineTo (xFor (c),
                           plot.getY() + juce::jmin (24.0f, frameAt (c).grDb) * grScale);
        grPath.lineTo (plot.getRight(), plot.getY());
        grPath.closeSubPath();
        g.setColour (colors::gainRed.withAlpha (0.35f));
        g.fillPath (grPath);
        g.setColour (colors::gainRed);
        g.strokePath (grPath, juce::PathStrokeType (1.0f));

        // Peak gain-reduction labels: local maxima at least 1 dB deep and
        // dominant within +-24 columns; the three deepest get a label.
        struct PeakLabel { int col; float gr; };
        std::vector<PeakLabel> peaks;
        for (int c = 8; c < columns - 8; ++c)
        {
            const float gr = frameAt (c).grDb;
            if (gr < 1.0f)
                continue;
            bool dominant = true;
            for (int k = juce::jmax (0, c - 24); k <= juce::jmin (columns - 1, c + 24); ++k)
                if (frameAt (k).grDb > gr + 1.0e-4f)
                {
                    dominant = false;
                    break;
                }
            if (dominant)
                peaks.push_back ({ c, gr });
        }
        std::sort (peaks.begin(), peaks.end(),
                   [] (const PeakLabel& a, const PeakLabel& b) { return a.gr > b.gr; });

        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        int drawn = 0;
        for (const auto& pk : peaks)
        {
            if (drawn++ >= 3)
                break;
            const float y = plot.getY() + juce::jmin (24.0f, pk.gr) * grScale;
            g.setColour (colors::gainRed);
            g.drawText (juce::String (pk.gr, 1),
                        (int) xFor (pk.col) - 18, (int) y + 2, 36, 12,
                        juce::Justification::centred);
        }
    }

    void paintHistogram (juce::Graphics& g, juce::Rectangle<float> plot)
    {
        double maxCount = 1.0;
        for (const auto v : histogram)
            maxCount = juce::jmax (maxCount, v);

        const float binW = plot.getWidth() / (float) numBins;
        g.setColour (colors::gainRed.withAlpha (0.8f));
        for (int b = 0; b < numBins; ++b)
        {
            const double v = histogram[(size_t) b];
            if (v <= 0.0)
                continue;
            const float h = (float) (std::log1p (v) / std::log1p (maxCount))
                            * plot.getHeight();
            g.fillRect (plot.getX() + (float) b * binW, plot.getBottom() - h,
                        juce::jmax (1.0f, binW - 1.0f), h);
        }

        // dB axis ticks every 6 dB of GR.
        g.setColour (colors::outline.withAlpha (0.7f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (int db = 0; db <= 24; db += 6)
        {
            const float x = plot.getX() + (float) db / 24.0f * plot.getWidth();
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (colors::textDim);
            g.drawText (juce::String (db), (int) x + 2, (int) plot.getBottom() - 14,
                        30, 12, juce::Justification::centredLeft);
            g.setColour (colors::outline.withAlpha (0.7f));
        }

        // Current GR needle.
        const float gr = processor.gainReductionDb.load();
        if (gr > 0.05f)
        {
            g.setColour (colors::text);
            const float x = plot.getX()
                            + juce::jlimit (0.0f, 1.0f, gr / 24.0f) * plot.getWidth();
            g.drawLine (x, plot.getY(), x, plot.getBottom(), 1.4f);
        }

        g.setColour (colors::textDim);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        const double pct = totalFrames > 0
            ? 100.0 * (double) limitedFrames / (double) totalFrames : 0.0;
        g.drawText ("limiting " + juce::String (pct, 1) + "% of the time",
                    plot.toNearestInt().removeFromTop (14),
                    juce::Justification::centredLeft);
    }

    ZenithAudioProcessor& processor;

    std::vector<ZenithAudioProcessor::ScopeFrame> history;
    int historyPos = 0;
    uint32_t readIndex = 0;

    static constexpr int numBins = 48; // 0.5 dB bins over 0..24 dB
    std::array<double, (size_t) numBins> histogram {};
    juce::int64 totalFrames = 0, limitedFrames = 0;

    int displayMode = 0;
    juce::TextButton scrollButton, zoomButton, histoButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainDisplay)
};

} // namespace znth
