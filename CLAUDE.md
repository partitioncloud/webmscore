# webmscore — notes for agents

This repo is a **fork of MuseScore Studio** (the whole upstream tree) plus the webmscore
library that builds `libmscore` to WebAssembly. Everything webmscore-specific lives in:

| Path | What |
|---|---|
| `web/` | the wasm library: `main.cpp` (all exported C functions), `score.cpp`, `wasmres.cpp`, plus the `audio/` and `importexport/` sub-projects and the CMake entry point |
| `web-public/` | the npm package (`webmscore4`): JS wrappers (`src/*.js`), rollup bundling, the published build artifacts |
| `web-example/` | the smoke tests (`npm start`, `npm run test-synth`) |
| `buildscripts/qt/` | prebuilt headless `offscreen` Qt platform plugin + the script that builds it |
| `.github/workflows/build.yml` | the fork's own CI (build → test → publish to npm); upstream's workflows are deleted |

Everything else is upstream MuseScore, patched in place where webmscore needs it.

The single branch is `webmscore` (main branch of the fork, no `master`).
Upstream is not configured as a remote by default: `git remote add upstream https://github.com/musescore/MuseScore.git`.

## Rebasing on a new MuseScore release

This is the recurring job. The history looks like:

```
* Build with Qt x / emscripten y      <- toolchain bump (only when needed)
* Webmscore: support MuseScore vX.Y.Z <- code fixes
* Rebase to vX.Y.Z                    <- squashed upstream update, single parent
* (previous webmscore HEAD)
```

The upstream history is squashed: "Rebase to vX.Y.Z" is a regular commit holding the
whole upstream update, and records the upstream commit it corresponds to in a
`MuseScore-Base: vX.Y.Z (<full hash>)` line. (Up to v4.6.5 these were merge commits.)

### 1. Merge with the *previous* release as merge base

Do **not** merge the tag directly: the true merge base is an old commit, which would
resurrect thousands of upstream changes as conflicts. Temporarily graft both sides onto
the previous release, so only the actual webmscore delta conflicts:

```sh
git fetch upstream --tags --no-recurse-submodules
BASE=$(git log -1 --format=%H --grep='^MuseScore-Base:' webmscore)          # last "Rebase to"
PREV=$(git log -1 --format=%B $BASE | sed -n 's/^MuseScore-Base: .*(\(.*\))$/\1/p')
git replace --graft vX.Y.Z^{commit} $PREV        # temporary: the new tag descends from PREV
git replace --graft $BASE $BASE^ $PREV           # temporary: so does webmscore
git merge-base webmscore vX.Y.Z^{commit}         # must print $PREV
git merge --squash vX.Y.Z^{commit}
# ... resolve ...
git commit                                       # "Rebase to vX.Y.Z" + MuseScore-Base line
git replace -d vX.Y.Z^{commit} $BASE             # remove the grafts
```

`git diff $PREV webmscore -- <file>` shows exactly what webmscore changed in a file —
the fastest way to decide a conflict.

### 2. Conflict categories (in rough order of frequency)

* **Deleted by us, modified by them** (`DU`): upstream CI workflows, root `CMakeLists.txt`,
  `SetupSndFile.cmake`… → keep deleted (`git rm`).
* **Files upstream moved**: re-apply the webmscore hunk at the new location, then `git rm`
  the old path. (4.6.5→4.7.5 moved `audio/worker/**` → `audio/engine/**`.)
* **UI/interaction stubs**: webmscore compiles a subset of the modules, so it comments out
  `ui`, `interaction`, `accessibility`, `midiinput`, `onlineSounds` usages, or guards them
  with `#ifndef __EMSCRIPTEN__`. Keep that style; when upstream adds code that uses these,
  guard it the same way rather than deleting it.
* **Resources**: webmscore loads fonts/instruments/styles from the (virtual) file system,
  not from Qt resources — paths lose the `:` prefix (`/fonts/...`, `/mpe/...`) and `.otf`/`.ttf`
  become `.woff2`. Qrc registration is disabled with `if (NOT EMSCRIPTEN)`.

### 3. Toolchain

Check what upstream uses for its own wasm build — webmscore must match, since it links
upstream's Qt:

```sh
grep EMSDK_VERSION buildscripts/ci/linux/setup.sh          # emsdk version
git show vX.Y.Z:.github/workflows/build_wasm.yml | grep -A6 "Install Qt"  # Qt version + arch
                                                           # (upstream's wasm workflow; the fork deletes it)
grep "find_package(Qt6" buildscripts/cmake/SetupQt6.cmake  # minimum Qt
```

Current: **Qt 6.10.2 `wasm_singlethread` + emsdk 4.0.7**, cross compiling also needs a host
Qt of the same version (`QT_HOST_PATH`). Versions live in `web/Makefile`,
`.github/workflows/build.yml` (`EM_VERSION`, `Qt6_VER`) and the README.

When the Qt version changes, the committed `offscreen` platform plugin must be rebuilt
(Qt does not ship it for wasm, and Qt's own `wasm` plugin requires a DOM):

```sh
QT6_ROOT=/path/to/Qt/6.10.2 ./buildscripts/qt/build_offscreen_plugin.sh
# then bump the version strings in buildscripts/qt/lib/cmake/Qt6Gui/*.cmake
```

## Building and testing

```sh
source /path/to/emsdk/emsdk_env.sh
QT=/path/to/Qt/6.10.2
cmake -S web -B web/build.release -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$QT/wasm_singlethread/lib/cmake/Qt6/qt.toolchain.cmake \
  -DQT_HOST_PATH=$QT/gcc_64
cmake --build web/build.release -j $(nproc)        # ~45 min from scratch, ccache helps a lot
```

Before the first build: `git submodule update --init --depth 1 thirdparty/brotli thirdparty/libsndfile`,
and copy `buildscripts/qt/*` into the Qt wasm dir (+ delete `Qt6QWasmIntegration*` from
`lib/cmake/Qt6Gui/`), see the README.

Smoke test (needs Node; the JS shims assume a browser-ish environment):

```sh
cp web/build.release/webmscore.lib.* web-public/
cd web-public && npm run build:js-only
cd ../web-example && rm -rf node_modules/webmscore4 && npm i ../web-public --install-links
npm test             # the test suite (node:test, web-example/test/*.test.js), ~1 min
npm start            # smoke test: export MusicXML/SVG/PNG/PDF/MXL/MIDI/OGG/positions/metadata
npm run test-synth   # smoke test: synthesis + per-track output params
```

`npm test` is the check that matters after a rebase; the two smoke tests only fail on a crash
(CI still runs them, for the exported files it uploads). Reinstall the package
(`rm -rf node_modules/webmscore4 && npm i ...`) after each rebuild: a plain `npm i` keeps the
old copy, since the version did not change.

Writing tests (`web-example/test/helpers.js` has the shared loaders/parsers):

* each test file runs in its own process (= its own wasm instance), one file at a time;
  state that leaks between tests (soundfont, current audio sequence) only leaks within a file
* assert invariants (page counts, magic bytes, RMS, contiguity of measures…), not exact bytes,
  so that tests survive upstream rebases
* known bugs are `{ todo: '…' }` tests: reported, but they do not fail the run — when one
  starts passing, drop the `todo`
* audio bugs that shape the tests: `saveAudio` only works once per instance (so formats are
  exported in child processes, `test/support/save-audio.js`)

## webmscore-specific switches

| Switch | Where | Meaning |
|---|---|---|
| `MUSE_WEB_LIB` (CMake) | `web/CMakeLists.txt` | this is the wasm *library*, not upstream's web *app*: real in-process audio engine, general RPC channel + soundfont controller, no audio driver, regular file system instead of memfs |
| `MUSE_AUDIO_INLINE_ENGINE` (define) | same | the code-level counterpart of the above |
| `ENGRAVING_NO_API` | same | skip the QML plugin api v1 (it needs `QProcess`) |
| `__EMSCRIPTEN__` | throughout `src/` | webmscore-only code paths in patched upstream files |

## Traps (each of these cost hours)

* **ioc: global vs context interfaces.** `globalIoc()` *is* `ioc(nullptr)` (same container),
  so as long as webmscore keeps using the default (null) context everywhere, both spellings
  register/resolve the same services — do keep it that way. The distinction is compile-time:
  `GlobalInject<T>` requires `MODULE_GLOBAL_INTERFACE`, `ContextInject<T>` requires
  `MODULE_CONTEXT_INTERFACE`. When upstream flips an interface from one to the other, the
  fix is the inject type (or dropping an unused inject), not the container.
  `resolve<T>()` never asserts: a service nobody registered is silently `nullptr`, and the
  crash comes later as a null virtual call.
* **Logging can be silent.** Qt/kors logger output from some call paths never reaches the
  Node console. When debugging, use `fprintf(stderr, ...); fflush(stderr);` — it always works.
* **Decoding wasm stack traces**: the `RuntimeError` frames are indices into
  `web/build.release/webmscore.lib.js.symbols`: `grep -m1 "^<index>:" …lib.js.symbols`.
  `null function or function signature mismatch` almost always means a null pointer was
  dereferenced through a virtual call (usually a service webmscore does not register).
* **The playback follows the *current* score.** The audio engine has a single sequence, the one
  of the global context's current notation. `makeCurrent()` (`web/main.cpp`) must run before any
  audio operation on a score; switching rebuilds the sequence from the project's audio settings,
  so params must be stored there (`web/audio/audioparams.cpp`), not only in the engine.
* **No threads, no event loop.** Anything asynchronous (promises, `async::processMessages`,
  the audio RPC queue) only advances when webmscore pumps it explicitly — see
  `Synth::getSequenceId()` in `web/audio/audiosynth.cpp` and the wait loops in
  `src/importexport/audioexport/internal/abstractaudiowriter.cpp`. If something "hangs",
  it is nearly always a missing pump, not a deadlock.
* **Do not pass `-fPIC`** in the wasm flags: with emsdk ≥ 4 it produces broken function
  pointers when linking against (non-PIC) Qt, which crashes inside `QGuiApplication`.
* **emscripten link flags age badly**: `EXTRA_EXPORTED_RUNTIME_METHODS` is gone (`_malloc`/
  `_free` belong in `EXPORTED_FUNCTIONS`), heap views such as `HEAPU8` must be exported
  explicitly, and valueless settings (`-s ASYNCIFY_IMPORTS`) are now errors.
* **Qt on wasm expects browser globals**: its event dispatcher calls `window.setTimeout` /
  `clearTimeout`, so the Node/worker shim in `web-public/src/utils.js` must provide them.

## File format compatibility

The score format version lives in `src/engraving/types/constants.h`
(`MSC_VERSION`, 470 for 4.7). It is exposed to JS as `WebMscore.version()` and printed by
the tests as *"supported file format version"* — a good first check that a rebase landed.
`web-public/package.json` tracks the MuseScore version (e.g. `4.7.5`); bump it (and
`package-lock.json`) with each rebase, since the npm release is published from
`release vX.Y.Z` commit messages (see the `publish` job in the CI workflow).
