# Toontown DNA Editor

An unofficial, open-source 3D/2D level editor for Toontown DNA scene files: DNA XML (`.xml`), classic
text DNA (`.dna`) and compiled DNA (`.pdna`). Native C++17 on the Panda3D engine (the engine Toontown runs on, so
`.bam`/`.egg` models and textures render as in the game), with a Dear ImGui interface and ImGuizmo gizmos.

> **Fan tool. Not affiliated with, endorsed by or connected to The Walt Disney Company.** "Toontown" and related
> names are trademarks of their respective owners and are used here only to describe what file formats the tool
> works with. **No Disney code, models, textures or other assets are included.** You point the editor at a game
> resources folder you already have (for example from a fan server you play on).

## Features

* 3D view and 2D top-down map, click/box select, drag to move, Shift+drag to rotate, move/rotate/scale gizmo
  anchored on the object, snapping, reopens the last file on startup (View menu toggle), undo/redo, duplicate/delete, prop palette with preview, visgroup toggles.
* Signs use fonts from the loaded storage, curved baselines, and per-letter transforms and colors. Edit a
  whole sign line and its font code in the scrollable Inspector; the file keeps its per-letter structure.
* Expand Inspector **Properties** to edit object and child properties, including newly placed objects,
  titles, textures, colors, and baseline dimensions. Property and sign edits support undo/redo.
* Animated props play in the scene and palette preview. Placing an animated preview keeps its animation.
* **F5** or **View > Walk through map** starts a Toon walker. Point at the 3D view: arrow keys move/turn,
  **Ctrl** jumps. F5 returns to the previous editing camera. The walker uses Toon models from your resources.
* **View > Camera speed** sets a saved wheel speed in feet per notch. Hold Shift for finer movement.
  Scrolling remains independent of map size. View menu toggles hide the map, Inspector, and other panels;
  **F2** opens the organized, scrollable shortcuts panel.
* Saves only what you changed: unchanged text, comments and ordering stay as they were.
* Formats (detected by extension, then by content):

| format | open | save |
|---|---|---|
| `.xml` DNA XML | yes | yes (span-patched) |
| `.dna` text DNA source | yes; comments and unknown nodes kept | yes (span-patched) |
| `.pdna` compiled DNA (incl. zlib-compressed) | yes (decompiled in memory) | yes (compiled; byte-identical to libpandadna's compiler) |
| password-encrypted `.pdna` | yes, with a password prompt | yes (re-encrypted with the same password) |

  File > **Open File...** / **Save As...** use native dialogs filtered to `.xml/.dna/.pdna`; Save As converts by
  the chosen extension. Headless: `dna-editor --convert IN OUT`.

## The resources folder

The editor needs a game `resources` folder (the one containing `phase_3`, `phase_4`, ... with models, maps and the
DNA storage files). It is resolved in this order:

1. `--res DIR` on the command line
2. the `DNAED_RES` environment variable
3. `resources=` in the config file: `%APPDATA%\DnaEditor\config.ini` on Windows,
   `~/.config/dna-editor/config.ini` on Linux
4. auto-detect: `./resources`, a `resources` folder next to or above the executable, and the usual launcher
   install locations
5. otherwise a folder picker opens at start-up

The Windows installer has a page to pick the folder (pre-filled when one is found). In the editor,
**File > Set Resources Folder...** changes it later. Storage files (`storage*.xml`, `storage*.dna` or
`storage*.pdna`) are picked per scene file by their usual names.

## Build

### Linux

Needs cmake, ninja, a C++17 compiler and a Panda3D SDK *with headers* (a `makepanda` build; the pip wheel has no
headers).

    cmake -S . -B build -G Ninja -DPANDA3D_ROOT=$HOME/panda3d/built
    ninja -C build
    ./build/app/dna-editor --res /path/to/resources [FILE]

`./run.sh [FILE]` builds if needed and starts it; `installer/install.sh [RESOURCES_DIR]` installs a launcher,
icons and a desktop entry for the current user.

### Windows

Needs Visual Studio 2022 (or Build Tools) with the C++ workload, CMake + Ninja (come with VS), and the Panda3D
1.11 SDK (default `C:\Panda3D-1.11.0-x64`, or set `PANDA3D_ROOT`).

    build_windows.bat            :: builds into build-win\
    build_windows.bat run [args] :: builds and starts

### Windows installer (Inno Setup)

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`), then in
PowerShell:

    installer\build_installer.ps1 [-Panda C:\Panda3D-1.11.0-x64] [-Version 0.0.4]

It builds the editor, stages the exe with the Panda3D runtime DLLs and the MSVC runtime, and writes
`build-win\DNA-Editor-Setup-v<version>.exe` (`installer/dna-editor.iss`). The installer is not
code-signed, so Windows SmartScreen shows an "unknown publisher" warning (More info > Run anyway).

## Command line

    dna-editor [--res DIR] [--size WxH] [--password PW] [FILE]
    dna-editor [--password PW] --convert IN OUT [--encrypt]

`FILE` is relative to the resources folder or an absolute path.

## Encrypted .pdna files

There is no documented standard encryption for `.pdna`. Servers that protect them usually wrap the whole file
with Panda3D's `encrypt_string` (OpenSSL `EncryptStream`); that is what the editor supports. A `.pdna` without the
`PDNA` magic that is not text is treated as encrypted; the password is accepted when the decrypted data starts
with the `PDNA` header.

* In the editor a modal prompt asks for the password (masked; OK / Cancel; a wrong password shows an error and
  asks again).
* For scripts: `--password PW` or the `DNAED_PDNA_PASSWORD` environment variable.
* The password is kept in memory for the running session only. It is never written to disk, the config file or
  the log.
* Saving an encrypted file re-encrypts it with the same password; `--convert IN OUT.pdna --encrypt` writes an
  encrypted copy.
* Other schemes can be added with `dna::setCipher()` in `src/dna/dna_formats.h`.

## Limits

* Older PDNA files may store sign lettering as baked BAM geometry. The editor renders and preserves that
  geometry in XML/DNA exports. Its original words and font are unavailable; entering a replacement line
  creates editable text. Compiling baked geometry into the newer PDNA layout is refused to prevent losing
  the signs. Save as XML/DNA or replace those sign lines before compiling PDNA.

* `.pdna` stores fixed-point values: 0.01 for positions/angles/scales, 1/255 for colours.
* Elements with no `.pdna` encoding (for example `curve_point` or server-specific tags) are left out of compiled
  output; the editor logs which.
* Decompiling then recompiling a storage `.pdna` keeps every entry but may change their order.
* Converting XML to `.dna` keeps the data but not XML comments.

## Verification

    cmake --build build
    ctest --test-dir build --output-on-failure

The format tests cover XML/DNA/PDNA sign editing, Unicode, quotes, spaces, pending additions/deletions,
property undo/redo, and empty storage files. Scene tests cover font registration, curved lettering,
per-letter properties, and zero-scale source objects without requiring game assets.

Optional real-resource checks use your local files (none are bundled):

    DNAED_TEST_RES=/path/to/resources ./build/tests/render_tests build/tests/render-tmp
    DNAED_TEST_OLD_PDNA=/path/to/older-map.pdna ./build/tests/render_tests build/tests/render-tmp

`DNAED_TEST_RES` exercises Toon assembly, movement, wall collisions, jumping and landing. Add
`DNAED_TEST_ANIM_MODEL` (resource-relative rig path) and `DNAED_TEST_ANIM_NAME` (animation filename without
extension) to check external prop animation binding and playback.

The GUI test injects actual mouse/key/text events, renders screenshots, and saves into a scratch directory:

    ./build/app/dna-editor --res /path/to/resources --selftest build/qa /path/to/map.pdna

On Linux, prefix it with `xvfb-run -a` to use a private display. It checks Inspector edits, undo/redo,
placement, animation controls, fractional scrolling, walking shortcuts, and save/reopen.

## Code layout

Each module has its own directory, CMakeLists and public header, with no cycles:

| module | what |
|---|---|
| `src/core` | log, timers, resources-folder config |
| `src/dna` | XML and `.dna` span-keeping parsers, `.pdna` compiler/decompiler, document model, storage selection, palette catalog, save |
| `src/commands` | undo/redo |
| `src/assets` | storage (code to model/texture), model cache |
| `src/render` | scene generation, views, gizmo |
| `src/ui` | panels and theme |
| `src/platform` | window, input, ImGui bridge, native dialogs |
| `app` | the editor and `main` |
| `tests` | engine-free `dna` tests with hand-written samples (`ctest --test-dir build`) |

## License

MIT, see [LICENSE](LICENSE). Third-party components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
