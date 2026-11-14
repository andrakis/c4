# libjs/vendor

Libraries c4 uses as they were published, for the sound card's General
MIDI synth (`../snd-gm.js`). Nothing here is changed but the two `import`
lines of `spessasynth_core.js`, which named the packages and here name the
files beside it (an AudioWorklet has no import map), and the dropped
`//# sourceMappingURL` lines.

| File | Package | Version | Licence |
|---|---|---|---|
| `spessasynth_core.js` | [spessasynth_core](https://github.com/spessasus/spessasynth_core) `dist/index.js` | 4.4.2 | Apache-2.0, `LICENSE.spessasynth_core` |
| `fflate.js` | [fflate](https://github.com/101arrowz/fflate) `esm/browser.js` | 0.8.3 | MIT, `LICENSE.fflate` |
| `stb-vorbis.js` | [stb-vorbis](https://github.com/spessasus/stb-vorbis) `dist/index.js` (stb_vorbis as WebAssembly, for `.sf3`) | 0.0.7 | Apache-2.0, `LICENSE.stb-vorbis` |

To update: `npm install --ignore-scripts spessasynth_core@VERSION` in a
scratch directory, copy the three files and their licences here, and make
the same two `import` lines relative again.

No SoundFont is here: the person uploads their own (`../README.md`,
Uploads).
