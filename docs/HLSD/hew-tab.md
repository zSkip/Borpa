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
  - `{"type":"plate","name":…,"data":<base64 3MF>,"nonSolid":<count>}` in answer to a send;
  - `{"type":"error","code":"not-ready"|"empty"|"export-failed","message":…}`.
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

On `plate` the host writes the file to `<temp>/borpa_hew/<name>.3mf` and loads it with
`LoadStrategy::LoadModel | LoadStrategy::Silence`, so the objects are added to the current project
and its presets are left alone, then switches to Prepare. `Silence` uses the saved geometry-import
preferences without showing the import-options dialog, and keeps the temporary file from becoming
the project file or the last-used folder; errors are still reported. Hew's 3MF export includes objects that are not
watertight solids; `nonSolid` counts them, and the host only logs it.

## Filaments in Hew

Hew's side is `hew/app/src/borpa/` (`filaments.ts`, `buildMaterial.ts`, `filamentScene.ts`).
A filament is a Hew material named after the preset and coloured like it, linked to the preset by
an attribute dictionary in the namespace `borpa.filament` (`preset`, `type`, `vendor`), so a
reopened document finds its filaments again by preset name, or by type when that preset is gone.
An Object's filament is its base material; per-face paint is not offered, and an import or a
document from before filaments is flattened to one filament per Object. Every operation that
makes an Object gives it the selected filament in the same undo step. Physical properties are not
stored in the document.

## Deliberately not shared

Hew keeps its own documents, undo history, settings and storage inside the web view's profile.
It reads the filament list and nothing else of the slicer's presets: not the printer bed or the
units. The slicer does not read `.hew` documents or keep a link from a plate object back to its
Hew source: what reaches Prepare is an ordinary mesh object, and every object prints in whatever
filament slot 1 holds.
