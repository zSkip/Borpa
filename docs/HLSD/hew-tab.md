# Hew tab — High Level Design

## Purpose and scope

The Hew tab embeds the Hew modeler — direct push/pull solid modelling, built as a Rust kernel
compiled to WebAssembly under a TypeScript/React UI — so a part can be drawn and sent to the
plate without leaving the slicer. Hew lives in the `hew/` submodule and is used as it is
shipped for the web; the slicer adds a host around it and one bridge call, and Hew adds a small
module (`hew/app/src/borpa/host.ts`) that answers that call.

Two things cross between them. **Send to Prepare** exports the open Hew document as a 3MF and
loads it into the current project like any imported file. And the slicer tells Hew which
filaments the selected printer can use: everything drawn in Hew is built in one of them, each
Object in one filament, shown in that filament's colour.

## Panel and page lifetime

`HewPanel` (`src/slic3r/GUI/HewPanel.cpp`) is a `LazyPage` with a prebuild order of -1: it is
never built on idle, only when the tab is first opened, because the page loads an ~11 MB
WebAssembly kernel. The tab sits between Home (and Design, when that is enabled) and Prepare;
`MainFrame::update_layout()` inserts it the same way it inserts Design.

A theme change does not reload the page (the panel handles `EVT_WEBVIEW_RECREATED`), since a
reload would discard the model being edited.

When `resources/web/hew/index.html` is missing the panel shows a message instead of a web view.
`scripts/build_hew.mjs` builds the bundle and copies it there; the directory is a build output
and is not committed.

## Serving the bundle

The bundle is served by `HewResourceHandler`, a `wxWebViewHandler` for the `hew` scheme that
maps a request path onto `<resources>/web/hew/` and refuses anything that could leave it (`..`
segments, drive letters, backslashes, absolute paths). It sets the MIME type from the file
extension; `application/wasm` in particular is required, because the page compiles its kernel
with `WebAssembly.compileStreaming`.

On Windows the WebView2 backend exposes a registered handler at the virtual host
`https://hew.wxsite/`, and the page is loaded from there rather than from a custom scheme or
`file://`: an https origin is a secure context, which the page's WebAssembly kernel and workers
need, and it gives the page a stable origin of its own for its local storage.
Handlers are registered right after the control is created, before its asynchronous
initialisation completes. On macOS and Linux the page is `hew://app/index.html`, and the handler
is registered in `WebView::CreateWebView()` before `Create()`: per view on WKWebView, once per
process on WebKitGTK, where URI schemes are global.

Hew's PWA service worker is removed from the copied bundle and never registered inside the
host, so an updated app never runs a cached older bundle.

## Bridge protocol

Messages are JSON strings with `"borpa": 1`; anything else is ignored.

- Before any page script runs, the host defines
  `window.__BORPA_HOST__ = { version, dark }`. Hew checks for it to know it is embedded.
- Page to host, through `window.wx.postMessage`:
  - `{"type":"ready"}` once the bridge is installed;
  - `{"type":"plate","name":…,"data":<base64 3MF>,"nonSolid":<count>,"filaments":{<material>:<preset>}}`
    in answer to a send;
  - `{"type":"error","code":"not-ready"|"empty"|"export-failed","message":…}`;
  - `sim.run` and `sim.cancel` (see Simulation messages).
- Host to page:
  - the button runs `window.__borpaHew.sendToPlate()`, or reports `not-ready` itself when the
    bridge is not installed yet;
  - `window.__borpaHew.setFilaments(list)` after every `ready` (a reloaded page has lost its
    list) and whenever the list changes. Each entry is
    `{name, type, vendor, colour, density, softeningTemp, slot}`: the preset's name, its
    `filament_type`, `filament_vendor` and `filament_colour` (`#RRGGBB`), `filament_density` in
    g/cm³ and `temperature_vitrification` in °C (`null` when the preset gives none), and the
    1-based project slot holding the preset (`null` for none). A preset in a slot shows the
    slot's colour.

The list holds every filament preset the filament combo boxes list: visible and compatible with
the selected printer. Presets change in the Prepare sidebar, the settings tabs and their dialogs,
so rather than hook each of those, the panel builds the list once a second while it is shown and
sends it when it differs from the last one sent. Hidden, it does nothing, and the first check
after the tab is shown again brings the page up to date.

On `plate` the host writes the file to `<temp>/borpa_hew/<name>.3mf` and reads it with
`Model::read_from_file` rather than `load_files`, because the objects are regrouped and given their
filaments before they reach the plater, and `load_files` would ask whether a stacked model is one
object. Hew names each core-spec `<base>` after its material, and a build material is named after
its filament preset, so `HewPlate::item_material_names` reads each build item's material name from
the file and the message's `filaments` map turns it into a preset. Each preset gets a project slot:
a slot already holding it is reused, a missing one is added after the physical slots
(`Sidebar::add_custom_filament`), and a preset the bundle does not have prints in slot 1. Objects
that touch (a vertex of one within 0.01 mm of the other's surface: a part drawn on another's face,
an inlay) become the parts of one object, merged with `Model::convert_multipart_object`; every
part, and every lone object, has its `extruder` set to its slot. `Plater::add_hew_objects` adds the
result centred on the bed in one undo step and the tab switches to Prepare. The project's other
presets are left alone. When the selected printer cannot print that many filaments at once (one
per extruder, unless it has a single-extruder multi-material unit), or a preset is unknown, the
objects are loaded anyway and a warning names the filaments concerned. Hew's 3MF export includes
objects that are not watertight solids; `nonSolid` counts them, and the host only logs it.

## Filaments in Hew

Hew's side is `hew/app/src/borpa/` (`filaments.ts`, `buildMaterial.ts`, `filamentScene.ts`).
A filament is a Hew material named after the preset and coloured like it, linked to the preset by
an attribute dictionary in the namespace `borpa.filament` (`preset`, `type`, `vendor`), so a
reopened document finds its filaments again by preset name, or by type when that preset is gone.
An Object's filament is its base material; per-face paint is not offered, and an import or a
document from before filaments is flattened to one filament per Object. Every operation that
makes an Object gives it the selected filament in the same undo step. Physical properties are not
stored in the document.

## The Sim tab

The Sim tab shows the same `HewPanel`, inserted into the book a second time the way Prepare and
Preview share the plater, so a study is set up on the model being edited rather than on a copy.
On every tab change `MainFrame` calls `HewPanel::show_as_sim()`, which hides Send to Prepare under
Sim and calls `window.__borpaHew.setMode("model" | "sim")`; the mode is also sent after every
`ready`, since the page may be built after the tab was chosen.

Simulations run outside the app's process. `HewSim` (`src/slic3r/GUI/HewSim.cpp`) takes a study
from the page and runs it on a worker thread through `src/borpa_sim`, a library with no
wxWidgets or libslic3r dependency that builds and tests on its own. The pipeline meshes the
Object with Gmsh, writes and runs CalculiX, checks the result and writes a result file the page
draws. Both programs are child processes, never linked: that isolates crashes, makes cancel a
kill, and keeps GPL programs out of Borpa's own binary. Each runs in a Job Object, so cancelling,
a watchdog stop, or Borpa exiting ends it and anything it started; with `PATH` cut down to its own
folder and the Windows folders, so it cannot load another program's DLLs; and CalculiX on one
thread, since its multi-threaded solver in these builds can return wrong answers with exit code
0 on a busy machine. Exit code 0 is never taken as success on its own: each stage checks its
output, and a static result must balance (the reactions at the supports cancel the applied
forces). The watchdog stops a run that has printed nothing and used no CPU for two minutes, or
that passes 30 minutes in all.

The solvers are looked up in `<install>/solvers/gmsh/` and `<install>/solvers/calculix/`, or in
the folder named by the `BORPA_SOLVERS_DIR` environment variable. The CMake cache entry
`BORPA_SOLVERS_SOURCE` names a folder to install as `solvers/`; `scripts/borpa_fetch_solvers.ps1`
downloads the pinned packages into one. A build without solvers still runs: the page is told
which are missing.

Job folders are `<temp>/borpa_sim/<job>/`, holding the mesher's and solver's input, output and
logs; folders older than seven days are removed when the page is first built.

## Simulation messages

- Host to page:
  - `setSimCapabilities({mesher, structural})` after every `ready`;
  - `simEvent(event)`, one of
    `{job, state:"running", stage:"mesh"|"solve"|"read", progress:0..1|null, message}`,
    `{job, state:"done", resultUrl, summary}` or
    `{job, state:"failed", code, message, logTail}`. Failure codes: `solver-missing`,
    `bad-study`, `mesh-failed`, `solve-failed`, `no-result`, `timeout`, `stalled`, `cancelled`.
- Page to host:
  - `{"type":"sim.run","job":…,"study":…,"geometry":…}` starts a job; one runs at a time and a
    new one cancels the old. `job` is `[A-Za-z0-9_-]{1,64}` and names the job folder. The study
    (version 1: linear static stress on one Object, with fixed faces and forces) and the
    geometry (the Object's welded surface, with a Hew face index per triangle, typed arrays in
    base64) are specified in `src/borpa_sim/Request.hpp`;
  - `{"type":"sim.cancel","job":…}`.

`summary` holds `nodes`, `elements`, `meshSize`, `maxDisplacement`, `maxVonMises`,
`reactionForce` and `wallSeconds`. The result is served at `sim/<job>/result.bsr` under the page's
origin; the handler serves that one file name from the job folders and nothing else from them.
Its layout is documented in `src/borpa_sim/ResultFile.hpp`: the mesh's outer surface, each
triangle tagged with its Hew face, and per-node fields (`displacement`, `vonMises`).

## Deliberately not shared

Hew keeps its own documents, undo history, settings and storage inside the web view's profile.
It reads the filament list and nothing else of the slicer's presets: not the printer bed or the
units. The slicer does not read `.hew` documents or keep a link from a plate object back to its
Hew source: what reaches Prepare is an ordinary mesh object, and every object prints in whatever
filament slot 1 holds.
