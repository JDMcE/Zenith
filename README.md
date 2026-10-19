# Zenith

A brickwall limiter plugin (VST3 / Standalone) built with JUCE 8. Part of the Equinox Audio
family alongside [Equinox](https://github.com/JDMcE/Equinox) (EQ),
[Meridian](https://github.com/JDMcE/Meridian) (compressor) and
[Horizon](https://github.com/JDMcE/Horizon) (reverb).

## Building

Requirements: CMake 3.22+ and a C++20 compiler.

Zenith shares code with the rest of the family through
[jdm_shared](https://github.com/JDMcE/jdm_shared), which must be checked out next to
this repository:

```
some-folder/
  Zenith/
  jdm_shared/
```

```
git clone https://github.com/JDMcE/Zenith.git
git clone https://github.com/JDMcE/jdm_shared.git
cd Zenith
git clone --depth 1 --branch 8.0.14 https://github.com/juce-framework/JUCE.git libs/JUCE
cmake -B build
cmake --build build --config Release
```

Targets:

- `zenith_VST3` / `zenith_Standalone` — the plugin
- `zenith_tests` — offline DSP verification harness (run the built exe; exits non-zero on failure)

## License

Zenith is free software, released under the GNU Affero General Public License v3.0
— see [LICENSE](LICENSE). It is built on [JUCE](https://juce.com), used under JUCE's
AGPLv3 open-source licence.
