#pragma once

#include <jdm/ui/LookAndFeel.h>

// Zenith's face of the shared family design language (jdm_shared): the
// common near-black blue-grey surfaces with Zenith's green control accent.
// Amber is the reserved hue — gain reduction only — and level metering is
// cyan, so all three family hues appear in their metering roles without
// colliding with the accent.

namespace znth
{

inline const jdm::colors::Palette zenithPalette {
    juce::Colour (0xff15181e), juce::Colour (0xff1d2129),
    juce::Colour (0xff242a34), juce::Colour (0xff313845),
    juce::Colour (0xffd8dce4), juce::Colour (0xff828a9a),
    juce::Colour (0xff4ec975), juce::Colour (0xff2b7a49),
    juce::Colour (0xffe09a44), juce::Colour (0xff46b5dc),
    juce::Colour (0xffd96a5a), juce::Colour (0xffb287d8)
};

inline const jdm::colors::Palette zenithPaletteHighContrast {
    juce::Colour (0xff090b0e), juce::Colour (0xff11141a),
    juce::Colour (0xff1b2029), juce::Colour (0xff5a6478),
    juce::Colour (0xfff4f7fc), juce::Colour (0xffbcc5d6),
    juce::Colour (0xff67e392), juce::Colour (0xff359c5b),
    juce::Colour (0xffffb45c), juce::Colour (0xff62cdf4),
    juce::Colour (0xffff8071), juce::Colour (0xffcfa5f4)
};

namespace colors
{
    using namespace jdm::colors;

    // Zenith's reserved hue means gain reduction (amber).
    inline juce::Colour& gainRed = jdm::colors::reserved;
}

struct LookAndFeel : jdm::LookAndFeel
{
    LookAndFeel() : jdm::LookAndFeel (zenithPalette, zenithPaletteHighContrast) {}
};

} // namespace znth
