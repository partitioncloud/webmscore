
# webmscore

> MuseScore's libmscore (the core library) in WebAssembly!  

## Features

* Parse `mscz` file data
* Get score metadata
* Export part score
* Generate music sheets in SVG/PNG/PDF format
* Generate MIDI
* Generate audio files in WAV, OGG, MP3, or FLAC format
* Synthesize raw audio frames, can be used in the Web Audio API 
* Export as MusicXML compressed/uncompressed
* Generate position information of measures or segments on the generated sheets
* Run inside a Web Worker thread

## Installation

> [!IMPORTANT]  
> The latest version available on npm corresponds to MuseScore 3, published by the [original LibreScore project](https://github.com/librescore/). For other versions, refer to (successively) [CarlGao4](https://github.com/CarlGao4/webmscore) (v4.3.2) [augustin64](https://github.com/augustin64/MuseScore) (v4.6.5) [partitioncloud](https://github.com/partitioncloud/webmscore) (this repo, v4.6.5-1 to current)

~~The package is available on npm: https://www.npmjs.com/package/webmscore~~

```sh
npm i webmscore
```

## Use webmscore

### Load in browsers

```html
<!-- using a CDN -->
<script src="https://cdn.jsdelivr.net/npm/webmscore/webmscore.js"></script>
<script>
    WebMscore.ready.then(async () => {
        const score = await WebMscore.load('mscz', msczdata)
    })
</script>
```

For latest browsers which support ES Modules

```js
import WebMscore from 'https://cdn.jsdelivr.net/npm/webmscore/webmscore.mjs'
```

### Run in Node.js directly

Minimum version: v8.9.0 with ES Modules support

The `--experimental-modules` flag is required for Node.js versions under 14,  
Also require `"type": "module"` in `package.json`

Node.js greater or equal to v18 is not supported. Node.js v16 is recommended.

```js
import WebMscore from 'webmscore'
WebMscore.ready.then(async () => {
    const score = await WebMscore.load('mscz', msczdata)
})
```

### Use a JavaScript bundler

*(TBD)*

### Load extra fonts

If your score sheet contains characters out of the range of the bundled [FreeFont](https://www.gnu.org/software/freefont/), those characters will be shown as tofu characters (`□` or `�`) in SVG/PNG/PDF files. Loading extra fonts is required.

webmscore can load any font format supported by [FreeType](https://www.freetype.org/freetype2/docs/index.html).

```js
const score = await WebMscore.load('mscz', msczdata, [...arrOfFontData])
```

> CJK fonts are no longer bundled inside webmscore since v0.6.0

### Load soundfont files

Loading a soudfont (sf2/sf3) file is required before generating/synthesizing audio.

```js
await score.setSoundFont(soudfontData)
```

Soudfonts can be found on [musescore.org website](https://musescore.org/en/handbook/soundfonts-and-sfz-files#list).

Example: (`FluidR3Mono_GM.sf3`)

```js
const soudfontData = new Uint8Array(
    await (
        await fetch('https://cdn.jsdelivr.net/gh/musescore/MuseScore@2.1/share/sound/FluidR3Mono_GM.sf3')
    ).arrayBuffer()
)
```

### Boost Mode

Sometimes you only want to process a bunch of score metadata, so drawing sheet images internally is a waste of time and system resource.

You can enable the Boost Mode by setting the `doLayout` parameter in `WebMscore.load` to `false`.

Example:

```js
const score = await WebMscore.load('mscz', msczdata, [], false)
const metadata = await score.metadata()
score.destroy()
```

webmscore's Boost Mode is about 3x faster than the batch converter feature (`-j`) of the musescore software, according to the [benchmark](./web-example/benchmark.js) result.

WebAssembly vs native C++ program!

## Compiling

> [!TIP]
> You can use [act](https://github.com/nektos/act) to download these dependencies and handle the build process for you, in a container.

1. Install essential tools like `make`, `cmake`, `ninja`, `llvm`, etc.

2. Install `emscripten` v4.0.7 using `emsdk`
https://emscripten.org/docs/getting_started/downloads.html

3. Get Qt6 for WebAssembly and apply patches

MuseScore 4.7 requires Qt >= 6.8, webmscore is built with Qt 6.10 (single threaded).
Cross compiling also needs a host Qt of the same version, for the build tools (moc, rcc, ...).

```sh
AQT_PREFIX=$PWD/build.qt6
Qt6_VER=6.10.2
Qt6_DIR=${AQT_PREFIX}/${Qt6_VER}/wasm_singlethread
# if you change the install directory or Qt version, remember to also change the
# `Qt6_VER` / `QT6_ROOT` variables in the `web/Makefile` file

# Download Qt using aqtinstall (https://github.com/miurahr/aqtinstall)
pip install aqtinstall
aqt install-qt linux  desktop ${Qt6_VER} linux_gcc_64      --outputdir ${AQT_PREFIX} --archives qtbase qtdeclarative qtsvg icu --modules qt5compat
aqt install-qt all_os wasm    ${Qt6_VER} wasm_singlethread --outputdir ${AQT_PREFIX} --archives qtbase qtdeclarative qtsvg     --modules qt5compat

# Apply patches, which
#   enable the prebuilt `offscreen` QPA platform plugin (https://doc.qt.io/qt-6/qpa.html), and
#   exclude the `wasm` QPA platform plugin, which requires a DOM
cp -r buildscripts/qt/* ${Qt6_DIR}
find ${Qt6_DIR}/lib/cmake/Qt6Gui/ -name 'Qt6QWasmIntegration*' -delete
```

The prebuilt `offscreen` plugin is committed in [buildscripts/qt](./buildscripts/qt);
run [build_offscreen_plugin.sh](./buildscripts/qt/build_offscreen_plugin.sh) to rebuild it
(needed when the Qt or emsdk version changes).

4. Checkout submodules

```sh
git submodule init
git submodule update
```

5. Compile `webmscore`

```sh
cd web-public
npm i
npm run build
```

Build artifacts are in the [web-public](./web-public) directory

## Browser Support 

All modern browsers which support [WebAssembly](https://caniuse.com/#feat=wasm) and [Async Functions](https://caniuse.com/#feat=async-functions)

| Name | Minimum Version |
|---|---|
| Chrome | 57 |
| Firefox | 53, 52 (non-ESR) |
| Edge | 16 (Fall Creators Update) |
| Safari | 11 |
| IE | NO! |
| Other browsers | I don't know! |

Only tested on the latest version of Chrome and Firefox.

## Examples

see files in the [web-example](./web-example) directory

```sh
cd ./web-example
npm i
npm start  # Node.js example
npm run start:browser  # browser example
```

## Tests

The test suite (Node.js ≥ 22) runs against the package installed in `web-example`:

```sh
cd ./web-example
npm i ../web-public --install-links
npm test
```

Tests live in [web-example/test](./web-example/test), one file per area (loading, exports, positions,
metadata, audio). Known bugs are kept as `todo` tests: they are reported but do not fail the run.

## Debugging

See [How to look up function names in the .symbols file?](https://github.com/LibreScore/webmscore/blob/web/CHANGELOG.md#0192---2021-01-25)

---

webmscore is part of the [LibreScore](https://github.com/LibreScore/) project.
