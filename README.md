# Borpa

Borpa is OrcaSlicer with the Hew 3D modeler built in. A "Hew" tab sits between
Home and Prepare. You model there and press "Send to Prepare" to put the model on
the build plate: it is exported as 3MF and added to the current project. From there
you slice as in OrcaSlicer.

## Modification notice

**Borpa is a modified version of OrcaSlicer, and it embeds a modified copy of Hew.
The modifications were made in November 2026.**

The modifications are:

- The Hew tab and its host bridge (`src/slic3r/GUI/HewPanel.*`).
- The application name and data folder are `Borpa`. Files and network identifiers
  that other software reads still say OrcaSlicer.
- A purple accent colour in place of OrcaSlicer's.
- No application update check against OrcaSlicer's servers.
- On the Hew side, a small host bridge (`hew/app/src/borpa/host.ts`), and no
  service worker when Hew runs embedded.

The full history of the changes is in this repository's git history and in that of
the `hew` submodule.

## Not affiliated

Borpa is not affiliated with, endorsed by, or supported by the OrcaSlicer project
or the Hew project. Please do not report Borpa problems to them.

## Credits

Borpa is built on the work of these projects:

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer), which is based on
- [Bambu Studio](https://github.com/bambulab/BambuStudio), which is based on
- [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is based on
- [Slic3r](https://github.com/slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

OrcaSlicer also draws on [SuperSlicer](https://github.com/supermerill/SuperSlicer)
by supermerill and on ideas from Cura.

The modeler is [Hew](https://github.com/hew3d/hew).

## Licence

Borpa is licensed under the GNU Affero General Public License, version 3 (see
`LICENSE.txt`).

OrcaSlicer, and therefore Borpa, includes a pressure advance calibration pattern
adapted from Andrew Ellis' generator, which is licensed under the GNU General
Public License, version 3, and is itself adapted from a generator by Sineos for
Marlin under the same licence. The optional Bambu networking plugin is based on
non-free libraries from Bambu Lab; it is optional and adds functions for Bambu Lab
printer users.

The Hew code in `hew/` is licensed under AGPL-3.0-only (`hew/LICENSE`) together
with the Hew Plugin API Exception (`hew/LICENSE-EXCEPTION`). The exception applies
to Hew's code only, not to the rest of Borpa. Hew's brand assets in `hew/brand/`
are licensed under CC BY 4.0 (`hew/brand/LICENSE`); that licence grants no
trademark rights to the name "Hew" or its mark.

The source for a given Borpa release is the tagged commit of this repository
together with the `hew` submodule commit it records.

## Building on Windows

Prerequisites:

- Visual Studio with the "Desktop development with C++" workload
- CMake
- Strawberry Perl
- Git
- Node.js with pnpm
- Rust (the toolchain named in `hew/rust-toolchain.toml`, with its wasm32 target)
  and wasm-pack

Steps:

1. Clone with the submodule:

   ```
   git clone --recurse-submodules https://github.com/zSkip/Borpa.git
   cd Borpa
   ```

2. Build the dependencies. This is slow and only needs doing once:

   ```
   .\build_win.bat -d
   ```

3. Build Hew's web bundle into `resources/web/hew`. Run `pnpm install` once in
   `hew/app` first. This must be done before the app build, which copies
   `resources`; without it the Hew tab shows a "not included in this build"
   message.

   ```
   node scripts/build_hew.mjs
   ```

4. Build the app:

   ```
   .\build_win.bat -s
   ```

   The result is `build\src\Release\orca-slicer.exe`.

If the checkout path contains a space, put the build trees elsewhere with
`--deps-dir` and `--build-dir`, and run from a shell where `cmake` and `perl` are
on PATH. For example:

```
.\build_win.bat -d --deps-dir C:\borpa-deps
.\build_win.bat -s --deps-dir C:\borpa-deps --build-dir C:\borpa-build
```

The executable is then `C:\borpa-build\src\Release\orca-slicer.exe`.
`.\build_win.bat -h` lists the other options.

macOS and Linux builds follow OrcaSlicer's build scripts but are untested for Borpa.

## Current limits

- Tested on Windows only.
- The executable is still named `orca-slicer.exe`, and there is no installer.
- Raster logos and icons (the application icon, the splash screen and a few Home
  page images) are still OrcaSlicer's.
- Each "Send to Prepare" adds objects to the plate; it does not replace objects
  sent earlier.
- Hew keeps its own look.
- Settings live in `%APPDATA%\Borpa` and are separate from those of an installed
  OrcaSlicer.

## More

How the Hew tab works is described in `docs/HLSD/hew-tab.md`.

After merging upstream OrcaSlicer changes, re-apply the accent colour with:

```
node scripts/borpa_recolor.mjs
```
