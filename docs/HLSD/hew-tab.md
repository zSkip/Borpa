# Hew tab — High Level Design

## Purpose and scope

The Hew tab embeds the Hew modeler — direct push/pull solid modelling, built as a Rust kernel
compiled to WebAssembly under a TypeScript/React UI — so a part can be drawn and sent to the
plate without leaving the slicer. Hew lives in the `hew/` submodule and is used as it is
shipped for the web; the slicer adds a host around it and one bridge call, and Hew adds a small
module (`hew/app/src/borpa/host.ts`) that answers that call.

The coupling is a single action: **Send to Prepare** exports the open Hew document as a 3MF and
loads it into the current project like any imported file. Nothing else crosses between the two.

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
- Host to page: the button runs `window.__borpaHew.sendToPlate()`, or reports `not-ready` itself
  when the bridge is not installed yet.

On `plate` the host writes the file to `<temp>/borpa_hew/<name>.3mf` and loads it with
`LoadStrategy::LoadModel` only, so the objects are added to the current project and its presets
are left alone, then switches to Prepare. Hew's 3MF export includes objects that are not
watertight solids; `nonSolid` counts them, and the host only logs it.

## Deliberately not shared

Hew keeps its own documents, undo history, settings and storage inside the web view's profile.
It does not read the slicer's presets, printer bed or units, and the slicer does not read `.hew`
documents or keep a link from a plate object back to its Hew source: what reaches Prepare is an
ordinary mesh object.
