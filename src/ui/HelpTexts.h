#pragma once

#include <juce_core/juce_core.h>

namespace znth::help
{

// One string per control, used three ways (v0.7): hover tooltips, the
// help-hint bar at the bottom of the window, and as the accessibility help
// text screen readers announce. Keep them one to two sentences, plain
// language, and mention interactions with other controls where they bite.

inline constexpr const char* gain =
    "Gain: input drive into the limiter. More gain = more limiting; combine with "
    "Unity Gain to audition the limiting without the loudness change.";
inline constexpr const char* ceiling =
    "Output Ceiling: the level the output never exceeds - guaranteed. With True "
    "Peak on, the ceiling is honoured in dBTP (inter-sample peaks included).";
inline constexpr const char* release =
    "Release: how quickly gain recovers after a peak. AUTO REL adds a "
    "programme-dependent dual envelope on top.";
inline constexpr const char* attack =
    "Attack: smoothing window applied to the gain envelope. Clamped to the "
    "lookahead - longer would break the no-overshoot guarantee.";
inline constexpr const char* lookahead =
    "Lookahead: how far ahead the limiter sees. Longer is smoother on transients "
    "and sets the reported latency.";
inline constexpr const char* linkTransient =
    "Transient Link: channel linking for the transient stage. 100% moves all "
    "channels together, 0% limits each independently. Surround links all "
    "full-range channels; the LFE always stays independent.";
inline constexpr const char* linkRelease =
    "Release Link: channel linking for the release stage - how much recovery is "
    "shared between channels.";
inline constexpr const char* styleBox =
    "Style: the limiting algorithm's character, from Transparent to Aggressive. "
    "Each remaps attack, release, and linking behaviour behind the same knobs.";
inline constexpr const char* autoRelease =
    "Auto Release: programme-dependent release - brief peaks recover fast, "
    "sustained material releases slowly.";
inline constexpr const char* dcFilter =
    "DC Filter: removes DC offset before limiting, so the ceiling headroom is "
    "not wasted on an inaudible offset.";
inline constexpr const char* unityGain =
    "Unity Gain: undoes the input gain at the output, so you judge the limiting "
    "at the incoming level instead of the louder result.";
inline constexpr const char* audition =
    "Audition Limiting: listen to the difference between input and output - "
    "exactly what the limiter removes. Silence means no limiting.";
inline constexpr const char* extSidechain =
    "External Side Chain: the side-chain input drives the gain computation, so "
    "grouped stems fed the same trigger duck identically. Falls back to the "
    "input when the side-chain bus is not connected.";
inline constexpr const char* oversampling =
    "Oversampling: linear-phase oversampling up to 32x for the limiting stage. "
    "Higher factors catch faster peaks at more latency and CPU.";
inline constexpr const char* truePeak =
    "True Peak: limits inter-sample peaks - the ceiling is honoured in dBTP and "
    "the limiter runs at 4x minimum regardless of the oversampling setting.";
inline constexpr const char* dither =
    "Dither: TPDF output dither for the target bit depth (16 or 24 bit). The "
    "very last processing step - the meters see exactly what leaves the plugin.";
inline constexpr const char* noiseShaping =
    "Noise Shaping: shapes the dither's quantization error - Flat TPDF, a simple "
    "slope, or psychoacoustic F/E weighting.";
inline constexpr const char* bypass =
    "Bypass: true click-free bypass (crossfaded, latency-aligned). Also on Shift+B.";
inline constexpr const char* undo =
    "Undo the last change (Ctrl+Z). Each A/B slot keeps its own history.";
inline constexpr const char* redo = "Redo (Ctrl+Shift+Z or Ctrl+Y).";

// Header: presets, A/B, layout.
inline constexpr const char* presetName =
    "Preset browser: search, tags, favorites, factory and user presets (P). "
    "Shows the current preset's name.";
inline constexpr const char* prevPreset = "Previous preset ([).";
inline constexpr const char* nextPreset = "Next preset (]).";
inline constexpr const char* slotA =
    "State A (A key). A/B keeps two complete states with independent undo histories.";
inline constexpr const char* slotB = "State B (B key).";
inline constexpr const char* copySlot =
    "Copy the active state over the other slot (C). Undoable in the destination's history.";
inline constexpr const char* compact =
    "Compact layout (K): hide the display and shrink the window.";
inline constexpr const char* helpButton =
    "Help: manual, keyboard shortcuts, hints bar, high contrast, text size.";
inline constexpr const char* cpu =
    "Processing load as a fraction of realtime. Rises with oversampling.";

// Display + meter strip (hint bar only - these aren't tooltip clients).
inline constexpr const char* mainDisplayText =
    "Level display: input (outline), output (filled), applied gain reduction from "
    "the top in amber with peak-GR labels. Buttons switch SCROLL / ZOOM (ceiling "
    "strip) / HISTO (GR histogram; click to reset).";
inline constexpr const char* meterStrip =
    "IN / GR / OUT meters with peak hold and clip indicators. Click the GR "
    "peak-hold line to reset it.";
inline constexpr const char* standardBox =
    "Loudness standard: sets the integrated target (-23 LUFS EBU R128, "
    "-24 LKFS ATSC A/85).";
inline constexpr const char* meterScale =
    "Meter scale for the IN/OUT meters, including K-System scales with a 0 VU "
    "reference line. A per-user view preference - presets never change it.";
inline constexpr const char* resetLoudness =
    "Restart the integrated loudness / LRA / max true peak measurement.";

// Keyboard shortcuts: one table drives both the F1 overlay and the manual,
// so they cannot drift apart.
struct Shortcut { const char* keys; const char* action; };
inline constexpr Shortcut shortcuts[] = {
    { "Ctrl+Z",                "Undo (active A/B slot)" },
    { "Ctrl+Shift+Z / Ctrl+Y", "Redo" },
    { "A / B",                 "Switch to state A / B" },
    { "C",                     "Copy current state to the other slot" },
    { "P",                     "Open / close the preset browser" },
    { "[ / ]",                 "Previous / next preset" },
    { "Shift+B",               "Toggle bypass" },
    { "K",                     "Toggle Compact layout" },
    { "H",                     "Toggle help hints bar" },
    { "F1",                    "Keyboard shortcuts overlay" },
    { "Esc",                   "Close overlay / browser" },
    { "Tab / Shift+Tab",       "Move keyboard focus between controls" },
    { "Arrow keys",            "Adjust the focused knob" },
};

// The manual ships inside the plugin binary (there is no installer yet at
// v0.7); "Open Manual" writes it next to the settings file and opens the
// default browser.
inline const char* manualHtml()
{
    return R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<title>Zenith Manual</title>
<style>
 body { font-family: Segoe UI, Helvetica, Arial, sans-serif; background:#15181e; color:#d8dce4;
        max-width: 60em; margin: 2em auto; padding: 0 1.5em; line-height: 1.55; }
 h1, h2, h3 { color: #4ec975; font-weight: 600; }
 h1 { border-bottom: 1px solid #313845; padding-bottom: .3em; }
 code, kbd { background:#242a34; border-radius:3px; padding:.1em .4em; color:#d8dce4; }
 table { border-collapse: collapse; margin: 1em 0; }
 th, td { border: 1px solid #313845; padding: .35em .7em; text-align: left; }
 th { background:#1d2129; }
 .gr { color:#e09a44; } .accent { color:#4ec975; } .cyan { color:#46b5dc; }
</style></head><body>

<h1>Zenith</h1>
<p>Zenith is a mastering-grade true peak limiter: a lookahead engine with a mathematical
no-overshoot guarantee, eight limiting styles, linear-phase oversampling up to 32x,
advanced dithering, a full BS.1770 loudness suite, stem-mastering side chain support and
5.1/7.1 surround. This manual covers every control; hover any control in the plugin (or
press <kbd>H</kbd> for the hints bar) for the short version in place.</p>

<h2>Quick start</h2>
<ol>
<li>Set the <b>Ceiling</b> to your delivery spec (streaming: typically -1&nbsp;dBTP with
    <b>TRUE PEAK</b> engaged).</li>
<li>Raise <b>Gain</b> until the amber <span class="gr">GR</span> trace shows limiting on
    the loud parts; watch the loudness suite for your target.</li>
<li>Pick a <b>Style</b> for the material and shape recovery with <b>Release</b> /
    <b>AUTO REL</b>.</li>
<li>Check what you are losing with <b>DELTA</b> (audition the removed signal), and level-
    match your judgement with <b>UNITY</b>.</li>
<li>Compare candidate settings with <b>A/B</b>; each slot keeps its own undo history.</li>
</ol>

<h2>Limiting styles</h2>
<table>
<tr><th>Style</th><th>Character</th></tr>
<tr><td>Transparent</td><td>Neutral reference: exactly your settings.</td></tr>
<tr><td>Punch</td><td>Short attack window, fast first-stage recovery; drums hit through.</td></tr>
<tr><td>Dynamic</td><td>Programme-dependent release, transient-forward.</td></tr>
<tr><td>Versatile</td><td>Slightly softened all-rounder with a short hold.</td></tr>
<tr><td>Aggressive</td><td>Fast everything - loudness first.</td></tr>
<tr><td>Modern</td><td>Smooth dual-stage release; EDM/pop sheen.</td></tr>
<tr><td>Bus</td><td>Slow, fully channel-linked, glue-like safety cap.</td></tr>
<tr><td>Safe</td><td>Maximum smoothing plus an extra 0.1 dB ceiling margin.</td></tr>
</table>

<h2>Main controls</h2>
<p><b>Gain</b> drives the input; <b>Ceiling</b> is the hard output bound the engine
guarantees (a safety clamp exists but provably never engages at a static ceiling).
<b>Release</b> sets recovery speed; <b>AUTO REL</b> layers a dual-envelope
programme-dependent recovery where brief peaks recover up to 3x faster and sustained
material up to 2x slower. <b>Attack</b> is the smoothing window on the gain envelope and
is clamped to <b>Lookahead</b>, which sets how far ahead the limiter sees (and the
reported latency).</p>
<p><b>TRANS LINK</b> and <b>REL LINK</b> link the channels separately for the transient
and release stages: 100% moves everything together, 0% limits channels independently.
In surround, all full-range channels form the link group and the LFE stays independent
in both directions (it is still limited itself). <b>DC</b> removes DC offset before
limiting. <b>UNITY</b> undoes the input gain at the output so you judge limiting at the
incoming level; <b>DELTA</b> plays exactly what the limiter removes.</p>

<h2>Output stage</h2>
<p><b>Oversample</b> runs the limiter at up to 32x with linear-phase FIR filters
(latency is reported to the host; watch the CPU readout). <b>TRUE PEAK</b> raises the
effective factor to at least 4x and honours the ceiling in dBTP so inter-sample peaks
cannot escape - the max <span class="cyan">TP</span> readout in the meter strip is
always measuring either way. <b>Dither</b> applies TPDF dither at 16 or 24 bit as the
very last step, with <b>Noise Shaping</b> options Flat, Slope, F-Weighted (Lipshitz)
and E-Weighted (Wannamaker).</p>

<h2>Display &amp; metering</h2>
<p>The display scrolls input (outline), output (filled) and the applied
<span class="gr">gain reduction</span> from the top, labelling the deepest recent GR
peaks. <b>ZOOM</b> shows the ceiling strip (ceiling-9&nbsp;dB to +3&nbsp;dB);
<b>HISTO</b> is an infinite-persistence histogram of GR depth (click to reset). The
meter strip shows IN/GR/OUT with peak hold and clip markers, and the loudness suite
reads Momentary, Short-term, gated Integrated and LRA per BS.1770-4/EBU R128 against
the selected standard's target (<b>EBU R128</b> -23 LUFS / <b>ATSC A/85</b> -24 LKFS),
plus max true peak. Meter scales include K-System (K-20/K-14/K-12) with 0 VU reference
lines; scale and standard are per-user view preferences, never preset state.</p>

<h2>Routing: side chain &amp; surround</h2>
<p><b>EXT SC</b> switches the detector to the external side-chain bus: for stem
mastering, feed every stem instance the same trigger (typically the stem sum) and they
all duck identically, so the balance between stems is preserved. The side chain is
deliberately outside the input gain. If the mains run hotter than the trigger, the
ceiling is still hard-enforced. Zenith runs mono, stereo and surround up to 7.1 with
the link topology described above; loudness weighting follows BS.1770-4 (surrounds
+1.5&nbsp;LU, LFE excluded).</p>

<h2>Presets, A/B, and undo</h2>
<p>The preset browser (<kbd>P</kbd>, or click the preset name) searches names, tags,
categories and descriptions; star any preset to collect it under <b>Favorites</b>.
Factory presets are built in; user presets are plain XML <code>.znth</code> files in
<code>Documents/Zenith/Presets</code> - friendly to backups and file sync, and portable
with <b>IMPORT</b>/<b>EXPORT</b>. <b>A/B</b> keeps two complete states with instant
lossless switching and independent undo per slot.</p>

<h2>MIDI Learn</h2>
<p>Right-click any knob and choose <b>MIDI Learn</b>, then move a controller: the next
CC received is mapped and applied immediately (one CC per parameter, one parameter per
CC). Mappings persist with the session, ride along when loading presets, and are
removed with <b>Remove MIDI CC</b> on the same menu.</p>

<h2>Keyboard shortcuts</h2>
<table><tr><th>Keys</th><th>Action</th></tr>
)HTML";
}

// The closing half of the manual: the shortcut rows are generated from the
// table above and glued between manualHtml() and manualHtmlFooter().
inline constexpr const char* manualHtmlFooter =
    "</table>\n<p>Zenith is part of the Equinox Audio family alongside Equinox (EQ) and "
    "Meridian (compressor).</p>\n</body></html>\n";

inline juce::String buildManual()
{
    juce::String html (manualHtml());
    for (const auto& s : shortcuts)
        html << "<tr><td><kbd>" << s.keys << "</kbd></td><td>" << s.action << "</td></tr>\n";
    html << manualHtmlFooter;
    return html;
}

} // namespace znth::help
