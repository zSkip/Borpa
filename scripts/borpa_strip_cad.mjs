#!/usr/bin/env node
// Removes OrcaSlicer's own Design (CAD) tab from the work tree. Borpa embeds the Hew modeler
// instead, so the Design tab's sources, solver dependency, tests, scripts, docs and icons are
// deleted while every `#ifdef SLIC3R_CAD` / `if (SLIC3R_CAD)` hook in shared files is kept as is,
// which keeps the patch to upstream small. Re-run it after merging upstream OrcaSlicer to delete
// the same paths again if the merge brought any of them back.
//
// What it does:
//   deletes the folders and files in PATHS and the icons in ICONS (resources/images/<name>.svg),
//   removes the deleted sources' lines from localization/i18n/list.txt (bytes and line endings of
//   the other lines are kept; the .pot/.po catalogs are not regenerated).
// What it only checks, and reports as WARNING without editing, because those hunks can be lost in
// a merge: SLIC3R_CAD defaults to OFF in CMakeLists.txt and deps/CMakeLists.txt, and both files
// still carry the Borpa FATAL_ERROR guard that stops a configure with SLIC3R_CAD=ON.
//
// It never touches hew/. Deletions are made in the work tree only; stage them yourself. A second
// run prints "Nothing to do."
//
// Usage: node scripts/borpa_strip_cad.mjs [--dry-run]

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const PATHS = [
  // Design tab sources and the SolveSpace solver dependency.
  'src/libslic3r/CAD',
  'src/slic3r/GUI/CAD',
  'deps/SLVS',
  // Scripts, tools and docs that only serve the Design tab.
  'scripts/CAD',
  'tools/orca_cad_mcp_bridge.py',
  'docs/CAD',
  'docs/HLSD/design-tab.md',
  '.claude/loopspec/sketch-focus-arbiter.spec.md',
  // Tests and their fixtures (the cad_recipe 3MF pass-through test in test_3mf.cpp is kept).
  'tests/libslic3r/test_caddocument.cpp',
  'tests/libslic3r/test_sketchconstraints.cpp',
  'tests/libslic3r/test_sketchedit.cpp',
  'tests/libslic3r/test_sketchimport.cpp',
  'tests/libslic3r/test_sketchinference.cpp',
  'tests/libslic3r/test_sketchprofile.cpp',
  'tests/libslic3r/test_slvs_constraints.cpp',
  'tests/slic3rutils/test_design_sketch_tool.cpp',
  'tests/data/cad_recipe_v3.bin',
  'tests/data/cad_recipe_v4.bin',
  'tests/data/cad_recipe_v5.bin',
  'tests/data/cad_brep_occt76.brep',
]

// Icons referenced only by the deleted sources (resources/images/<name>.svg).
const ICONS = `
  color_palette toolbar_add_plate_bodies toolbar_add_plate_bodies_dark
  design_arc3pt design_arc_center design_array design_bed design_boolean design_bspline
  design_c_angle design_c_coincident design_c_collinear design_c_concentric design_c_diameter
  design_c_dist_x design_c_dist_y design_c_equal design_c_equal_radius design_c_fix
  design_c_horizontal design_c_midpoint design_c_parallel design_c_perpendicular design_c_radius
  design_c_sym_h design_c_sym_v design_c_symmetric design_c_tangent design_c_vertical
  design_chamfer design_check design_circle design_circle2pt design_circle3pt design_constrain
  design_construction design_crect design_cut design_delete design_dimension design_draft
  design_dressup design_edit design_ellipse design_ellipse_arc design_export design_extend
  design_extrude design_eye design_eye_off design_filletedge design_hole design_import_mesh
  design_line design_loft design_mirror design_move design_movedown design_moveup design_new
  design_new_sketch design_offset design_pattern design_plane design_point design_polararray
  design_polygon design_polyline design_rect design_rect_oblique design_rect_rounded
  design_revolve design_rib design_rotate design_scale design_select design_shell design_sketch
  design_slot design_slot_arc design_step design_surface design_svg design_sweep
  design_tangentarc design_text design_thicken design_thread design_trim design_zoom
`.trim().split(/\s+/).map(name => `resources/images/${name}.svg`)

// Lines of localization/i18n/list.txt naming a deleted source.
const LIST_TXT = 'localization/i18n/list.txt'
const LIST_RE = /^src\/(libslic3r|slic3r\/GUI)\/CAD\//

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const dryRun = process.argv.includes('--dry-run')
const verb = dryRun ? 'would remove' : 'removed'

const abs = rel => {
  if (rel === 'hew' || rel.startsWith('hew/')) throw new Error(`refusing to touch ${rel}`)
  return path.join(repo, ...rel.split('/'))
}

const countFiles = dir => fs.readdirSync(dir, { withFileTypes: true })
  .reduce((n, e) => n + (e.isDirectory() ? countFiles(path.join(dir, e.name)) : 1), 0)

let changes = 0
let warnings = 0
let errors = 0

for (const rel of [...PATHS, ...ICONS]) {
  try {
    const p = abs(rel)
    const st = fs.lstatSync(p, { throwIfNoEntry: false })
    if (!st) continue
    const n = st.isDirectory() ? countFiles(p) : 0
    const what = st.isDirectory() ? `${rel}/ (${n} file${n === 1 ? '' : 's'})` : rel
    if (!dryRun) {
      fs.rmSync(p, { recursive: true, force: true })
      // Drop a folder the deletion left empty (.claude/loopspec); git does not track it anyway.
      const parent = path.dirname(p)
      if (parent !== repo && fs.readdirSync(parent).length === 0) fs.rmdirSync(parent)
    }
    console.log(`${verb} ${what}`)
    changes++
  } catch (err) {
    console.error(`${rel}: ${err.message}`)
    errors++
  }
}

try {
  const p = abs(LIST_TXT)
  // latin1 maps every byte to one char and back, so the file's bytes and CRLFs survive unchanged.
  const lines = fs.readFileSync(p).toString('latin1').split(/(?<=\n)/)
  const kept = lines.filter(l => !LIST_RE.test(l))
  if (kept.length !== lines.length) {
    for (const l of lines) if (LIST_RE.test(l)) console.log(`${verb} ${LIST_TXT}: ${l.trim()}`)
    if (!dryRun) fs.writeFileSync(p, Buffer.from(kept.join(''), 'latin1'))
    changes++
  }
} catch (err) {
  console.error(`${LIST_TXT}: ${err.message}`)
  errors++
}

// Hunks a merge can lose: the OFF defaults and the FATAL_ERROR guards.
const warn = msg => { console.error(`WARNING: ${msg}`); warnings++ }
const OPTION_RE = /^\s*option\s*\(\s*SLIC3R_CAD\s+"(?:[^"\\]|\\.)*"\s+(\S+?)\s*\)/m
const checks = [
  { file: 'CMakeLists.txt', guard: /if\s*\(\s*SLIC3R_CAD\s+AND\s+NOT\s+EXISTS\s+"?\$\{CMAKE_CURRENT_SOURCE_DIR\}\/src\/libslic3r\/CAD\/CadDocument\.cpp"?\s*\)\s*message\s*\(\s*FATAL_ERROR/ },
  { file: 'deps/CMakeLists.txt', guard: /if\s*\(\s*SLIC3R_CAD\s+AND\s+NOT\s+EXISTS\s+"?\$\{CMAKE_CURRENT_LIST_DIR\}\/SLVS\/SLVS\.cmake"?\s*\)\s*message\s*\(\s*FATAL_ERROR/, before: /include\s*\(\s*SLVS\/SLVS\.cmake\s*\)/ },
]
for (const { file, guard, before } of checks) {
  let text
  try {
    text = fs.readFileSync(abs(file), 'utf8')
  } catch (err) {
    console.error(`${file}: ${err.message}`)
    errors++
    continue
  }
  const opt = text.match(OPTION_RE)
  if (!opt) warn(`${file}: option(SLIC3R_CAD ...) not found.`)
  else if (!/^(OFF|0|FALSE|NO|N)$/i.test(opt[1])) warn(`${file}: SLIC3R_CAD defaults to ${opt[1]}; set it back to OFF.`)
  const g = text.match(guard)
  if (!g) warn(`${file}: the Borpa FATAL_ERROR guard for SLIC3R_CAD is missing.`)
  else if (before) {
    const inc = text.match(before)
    if (inc && inc.index < g.index) warn(`${file}: the Borpa FATAL_ERROR guard must come before include(SLVS/SLVS.cmake).`)
  }
}

if (!changes) console.log('Nothing to do.')
else if (dryRun) console.log('(dry run, nothing changed)')
if (warnings) console.error(`${warnings} warning(s).`)
if (errors) {
  console.error(`${errors} path(s) could not be processed.`)
  process.exit(1)
}
