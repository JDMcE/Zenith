#pragma once

#include <jdm/presets/PresetManager.h>
#include "../Parameters.h"

namespace znth
{

using jdm::PresetManager;
using jdm::PresetInfo;

// Zenith's preset ecosystem configuration: file format identity plus the
// factory library, handed to the shared jdm::PresetManager. Presets without
// a MIDIMAP child inherit the session's current mappings on load (loading a
// sound shouldn't silently unplug the controller). State schema mirrors
// ZenithAudioProcessor::stateVersion.
inline jdm::PresetProductConfig makePresetConfig()
{
    jdm::PresetProductConfig c;
    c.productName = "Zenith";
    c.extension = "znth";
    c.presetTag = juce::Identifier ("ZenithPreset");
    c.stateTag = juce::Identifier ("ZenithState");
    c.stateVersion = 1;
    c.inheritChildTrees = { "MIDIMAP" };

    c.factory = {
        { "Init", "Basics", "default;clean",
          "Clean starting point: Transparent style, unity gain, -0.3 dB ceiling.",
          {} },

        { "Transparent Master", "Mastering", "master;transparent;clean",
          "Neutral catch-only limiting for material that is already balanced: "
          "slow release, generous lookahead, nothing added.",
          { { id::style, (float) Style::transparent }, { id::gain, 2.0f },
            { id::release, 400.0f }, { id::lookahead, 5.0f },
            { id::attack, 5.0f }, { id::autoRelease, 1.0f } } },

        { "Streaming Safe", "Mastering", "streaming;ceiling;safe;true peak",
          "Safe style with True Peak limiting at a -1.0 dBTP ceiling - the "
          "streaming-platform delivery spec.",
          { { id::style, (float) Style::safe }, { id::ceiling, -1.0f },
            { id::truePeak, 1.0f }, { id::release, 250.0f },
            { id::lookahead, 8.0f }, { id::autoRelease, 1.0f } } },

        { "Loud & Proud", "Mastering", "loud;aggressive;slam",
          "Aggressive loudness push: drive in, fast recovery, watch the meters.",
          { { id::style, (float) Style::aggressive }, { id::gain, 8.0f },
            { id::release, 50.0f }, { id::attack, 0.5f },
            { id::lookahead, 1.5f } } },

        { "Modern Sheen", "Mastering", "modern;pop;edm",
          "Smooth dual-stage release tuned for dense modern productions.",
          { { id::style, (float) Style::modern }, { id::gain, 5.0f },
            { id::release, 120.0f }, { id::lookahead, 3.0f } } },

        { "Punchy Drums", "Tracking & Bus", "drums;punch;transient",
          "Short attack window lets the hit through and clamps the ring.",
          { { id::style, (float) Style::punch }, { id::gain, 4.0f },
            { id::release, 60.0f }, { id::attack, 0.8f },
            { id::lookahead, 1.0f } } },

        { "Bus Glue Cap", "Tracking & Bus", "bus;glue;mix",
          "Fully linked, slow bus limiting - a safety cap that moves as one.",
          { { id::style, (float) Style::bus }, { id::release, 300.0f },
            { id::attack, 8.0f }, { id::lookahead, 6.0f },
            { id::autoRelease, 1.0f } } },

        { "Vocal Cap", "Tracking & Bus", "vocal;cap;smooth",
          "Dynamic style rides sustained phrases gently and still catches plosives.",
          { { id::style, (float) Style::dynamic }, { id::gain, 3.0f },
            { id::release, 150.0f }, { id::attack, 3.0f },
            { id::lookahead, 4.0f } } },

        { "Stem Safety", "Utility", "stems;safety;unity",
          "Unity Gain monitoring with a wide-open ceiling: hear exactly what "
          "the limiter would take off each stem.",
          { { id::style, (float) Style::transparent }, { id::unityGain, 1.0f },
            { id::release, 200.0f }, { id::lookahead, 5.0f },
            { id::autoRelease, 1.0f } } },

        { "Delta Check", "Utility", "delta;audition;qc",
          "Audition Limiting engaged: listen to what is being removed, not "
          "what remains. Great for release-time QC.",
          { { id::audition, 1.0f }, { id::gain, 6.0f },
            { id::release, 100.0f } } },

        // --- v0.7 factory expansion: full style coverage + genre/duty set ---

        { "Versatile Glue", "Mastering", "versatile;allround;master",
          "The softened all-rounder: a short transient hold keeps it musical "
          "on almost any full mix without a strong signature.",
          { { id::style, (float) Style::versatile }, { id::gain, 3.0f },
            { id::release, 200.0f }, { id::lookahead, 4.0f },
            { id::autoRelease, 1.0f } } },

        { "Broadcast -23", "Broadcast", "broadcast;ebu;r128;true peak;safe",
          "EBU R128 delivery: Safe style at a -1 dBTP true-peak ceiling with "
          "the loudness suite set to -23 LUFS. Set input gain to hit target.",
          { { id::style, (float) Style::safe }, { id::ceiling, -1.0f },
            { id::truePeak, 1.0f }, { id::release, 300.0f },
            { id::lookahead, 8.0f }, { id::autoRelease, 1.0f } } },

        { "Podcast Dialog", "Broadcast", "podcast;dialog;voice;speech;atsc",
          "Dynamic style tuned for spoken word: rides sustained speech gently, "
          "catches plosives, leaves room for -24 LKFS ATSC delivery.",
          { { id::style, (float) Style::dynamic }, { id::gain, 4.0f },
            { id::release, 180.0f }, { id::attack, 3.0f },
            { id::lookahead, 4.0f }, { id::dcFilter, 1.0f },
            { id::autoRelease, 1.0f } } },

        { "Trap Slam", "Mastering", "trap;hiphop;loud;aggressive;808",
          "Aggressive maximum-loudness push for modern hip-hop: hard, fast, "
          "808s held tight. Watch the true-peak readout.",
          { { id::style, (float) Style::aggressive }, { id::gain, 10.0f },
            { id::ceiling, -0.5f }, { id::release, 40.0f },
            { id::attack, 0.4f }, { id::lookahead, 1.2f } } },

        { "Acoustic Gentle", "Mastering", "acoustic;jazz;classical;transparent;gentle",
          "Catch-only transparency for dynamic acoustic material: a whisper of "
          "limiting on the loudest peaks and nothing else.",
          { { id::style, (float) Style::transparent }, { id::gain, 1.0f },
            { id::ceiling, -0.5f }, { id::release, 500.0f },
            { id::attack, 6.0f }, { id::lookahead, 6.0f },
            { id::autoRelease, 1.0f } } },

        { "Stem Bus Link", "Tracking & Bus", "stems;sidechain;ext;bus;link",
          "External side chain feeding grouped stems: point every instance at "
          "the same trigger bus and the stems duck identically, preserving the "
          "balance. Bus style, fully linked.",
          { { id::style, (float) Style::bus }, { id::extSidechain, 1.0f },
            { id::release, 250.0f }, { id::lookahead, 5.0f },
            { id::autoRelease, 1.0f } } },
    };
    return c;
}

} // namespace znth
