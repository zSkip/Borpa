#!/usr/bin/env node
// Applies Borpa's purple accent palette in place of OrcaSlicer's teal accent family, in the text
// resources (SVG icons, web pages) and in the plain colour notations of the C++ GUI sources.
// Re-run it after merging upstream OrcaSlicer to re-theme files that came in with the merge.
//
// What it rewrites, for every colour in COLOURS below:
//   #RRGGBB and #RRGGBBAA (any case; the alpha byte is kept),
//   0xRRGGBB (C++ sources only),
//   wxColour(r, g, b) / wxColor / ImColor / IM_COL32 / rgb() / rgba() with decimal channels
//   (the alpha argument and the original spacing are kept).
// A hex colour keeps the letter case of the colour it replaces; a source written only in digits
// (#009688) becomes upper case, so the SVG icons and the C++ keys that recolour them at load time
// (BitmapCache::load_svg) keep matching byte for byte.
//
// What it does not rewrite, and is maintained by hand: float colours (ImVec4, ColorRGB(A),
// `150 / 255.f`), the ABGR constant in BitmapCache::load_from_svg_file_change_color, the dark-mode
// partners in the StateColor map comments, and one-off shades whose role depends on the call
// site (Bambu-green buttons and tints, viewport overlays).
//
// Files are read and written as raw bytes, so encoding, BOM and line endings are untouched, and a
// file is only written when something in it changed. A second run changes nothing.
//
// Usage: node scripts/borpa_recolor.mjs [--dry-run]

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// Borpa palette, lightest to darkest.
const P1 = '#E4D8E9'
const P2 = '#CCB8DD'
const P3 = '#AB96C6'
const P4 = '#8C74B5'
const P5 = '#745FA8'
// Pressed / darker accent: the palette has nothing darker than P5, so this one shade is derived.
const P5_PRESSED = '#624F96'
// P5 blended over the dark-mode window background #2D2D31 at 25% and 10%.
const DARK_TINT_25 = '#3F3A4F'
const DARK_TINT_10 = '#34323D'

// Orca colour -> Borpa colour. `only` limits an entry to paths starting with one of the prefixes,
// for shades that are accent in icons but data elsewhere (flush tables, device states).
const COLOURS = [
  // Accent: buttons, selected tabs, icons, checkmarks, links.
  { role: 'accent', from: '#009688', to: P5 },
  { role: 'accent (near duplicate)', from: '#009687', to: P5 },
  { role: 'accent (near duplicate, logo)', from: '#009789', to: P5 },
  { role: 'accent hover / lighter', from: '#26A69A', to: P4 },
  { role: 'accent pressed / darker', from: '#00897B', to: P5_PRESSED },
  { role: 'accent, dark mode', from: '#00675B', to: P5 },
  { role: 'accent hover, dark mode', from: '#008172', to: P4 },
  { role: '25% accent tint on white (checked / selected background)', from: '#BFE1DE', to: P2 },
  { role: '10% accent tint on white (focused / hover background)', from: '#E5F0EE', to: P1 },
  { role: '25% accent tint, dark mode', from: '#223C3C', to: DARK_TINT_25 },
  { role: '10% accent tint, dark mode', from: '#283232', to: DARK_TINT_10 },
  // Other accent shades: the palette colour nearest in lightness, after the lightness shift the
  // rows above imply (teal L* 55.7 -> P5 45.3, 61.7 -> P4 53.4, 87.1 -> P2 77.5, 93.9 -> P1 87.7).
  { role: 'lighter accent (checked + hover, SVG #00FF00 stand-in)', from: '#52C7B8', to: P3 },
  { role: 'focus border, dark mode button', from: '#22BFB0', to: P3 },
  { role: 'focus border, light mode button', from: '#00FFD4', to: P2 },
  { role: 'accent hover in icons', from: '#33ABA1', to: P4 },
  { role: 'accent focus in checkbox icons', from: '#4DB6AC', to: P4 },
  { role: 'link text on the home page', from: '#00FFEA', to: P2 },
  { role: 'selected row, dark web dialogs', from: '#244945', to: DARK_TINT_25 },
  { role: 'link text, dark web dialogs', from: '#5EEAD4', to: P2 },
  { role: 'accent text, dark web dialogs', from: '#8DE5D6', to: P2 },
  { role: 'accent tint in AMS pipe icons', from: '#EBF9F0', to: P1, only: ['resources/images/'] },
  { role: 'Bambu green accent in icons', from: '#00AE42', to: P4, only: ['resources/images/'] },
]

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const ROOTS = ['src', 'resources']
const EXCLUDED = [
  // Never touched by the theme.
  'hew', 'deps', 'deps_src', 'localization', 'tests', '.git',
  'resources/profiles', 'resources/web/hew',
  // Colour data, not UI: filament / extrusion colours, flush tables, colour palettes, and a
  // third-party page.
  'src/libslic3r',
  'resources/flush', 'resources/filament_mixing',
  'resources/web/dialog/ColorPickerDialog/palettes.js',
  'resources/web/elegoolink',
]
const CPP = new Set(['.cpp', '.hpp', '.h', '.c', '.cc', '.mm', '.m'])
const TEXT = new Set([...CPP, '.svg', '.css', '.js', '.mjs', '.html', '.htm', '.json', '.ini', '.txt', '.md', '.xml'])

const dryRun = process.argv.includes('--dry-run')

const rgbOf = hex => { const v = parseInt(hex.slice(1), 16); return [v >> 16, (v >> 8) & 255, v & 255] }
const entries = COLOURS.map(c => ({ ...c, hex: c.from.slice(1).toUpperCase(), rgb: rgbOf(c.from), toHex: c.to.slice(1), toRgb: rgbOf(c.to) }))

const applies = (entry, rel) => !entry.only || entry.only.some(p => rel.startsWith(p))

// Upper case unless the replaced colour was written in lower-case letters.
const caseLike = (orig, hex) => (/[a-f]/.test(orig) && !/[A-F]/.test(orig) ? hex.toLowerCase() : hex.toUpperCase())

const HEX_RE = /(?<![&\w])#([0-9a-fA-F]{6})([0-9a-fA-F]{2})?(?![0-9a-fA-F])/g
const OXHEX_RE = /(?<!\w)0x([0-9a-fA-F]{6})(?![0-9a-fA-F])/g
const DEC_RE = /\b(wxColou?r|ImColor|IM_COL32|rgba?)(\s*\(\s*)(\d{1,3})(\s*,\s*)(\d{1,3})(\s*,\s*)(\d{1,3})(?![\d.])/gi

function recolour(text, rel) {
  const active = entries.filter(e => applies(e, rel))
  if (!active.length) return { text, count: 0 }
  const byHex = new Map(active.map(e => [e.hex, e]))
  const byRgb = new Map(active.map(e => [e.rgb.join(','), e]))
  let count = 0
  text = text.replace(HEX_RE, (m, hex, alpha = '') => {
    const e = byHex.get(hex.toUpperCase())
    if (!e) return m
    count++
    return '#' + caseLike(hex, e.toHex) + alpha
  })
  if (CPP.has(path.extname(rel).toLowerCase())) {
    text = text.replace(OXHEX_RE, (m, hex) => {
      const e = byHex.get(hex.toUpperCase())
      if (!e) return m
      count++
      return '0x' + caseLike(hex, e.toHex)
    })
  }
  text = text.replace(DEC_RE, (m, fn, open, r, s1, g, s2, b) => {
    const e = byRgb.get(`${+r},${+g},${+b}`)
    if (!e) return m
    count++
    const [nr, ng, nb] = e.toRgb
    return fn + open + nr + s1 + ng + s2 + nb
  })
  return { text, count }
}

const isExcluded = rel => /^build/i.test(rel) || EXCLUDED.some(x => rel === x || rel.startsWith(x + '/'))

function* walk(dir) {
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const abs = path.join(dir, ent.name)
    const rel = path.relative(repo, abs).split(path.sep).join('/')
    if (isExcluded(rel)) continue
    if (ent.isDirectory()) yield* walk(abs)
    else if (ent.isFile() && TEXT.has(path.extname(ent.name).toLowerCase())) yield { abs, rel }
  }
}

// Report by the first two path components (resources/images, src/slic3r, ...).
const areaOf = rel => rel.split('/').slice(0, 2).join('/')

const stats = new Map()
let errors = 0
for (const root of ROOTS) {
  let files
  try {
    files = [...walk(path.join(repo, root))]
  } catch (err) {
    console.error(`Could not list ${root}: ${err.message}`)
    errors++
    continue
  }
  for (const { abs, rel } of files) {
    try {
      const bytes = fs.readFileSync(abs)
      if (bytes.includes(0)) continue // binary despite its extension
      // latin1 maps every byte to one char and back, so non-ASCII bytes survive unchanged.
      const { text, count } = recolour(bytes.toString('latin1'), rel)
      if (!count) continue
      const out = Buffer.from(text, 'latin1')
      if (out.equals(bytes)) continue
      if (!dryRun) fs.writeFileSync(abs, out)
      const s = stats.get(areaOf(rel)) ?? { files: 0, replacements: 0 }
      s.files++
      s.replacements += count
      stats.set(areaOf(rel), s)
    } catch (err) {
      console.error(`${rel}: ${err.message}`)
      errors++
    }
  }
}

const rows = [...stats.entries()].sort(([a], [b]) => a.localeCompare(b))
const total = rows.reduce((t, [, s]) => ({ files: t.files + s.files, replacements: t.replacements + s.replacements }), { files: 0, replacements: 0 })
const width = Math.max(5, ...rows.map(([a]) => a.length))
console.log(`${'area'.padEnd(width)}  files  replacements${dryRun ? '  (dry run)' : ''}`)
for (const [area, s] of rows) console.log(`${area.padEnd(width)}  ${String(s.files).padStart(5)}  ${String(s.replacements).padStart(12)}`)
console.log(`${'total'.padEnd(width)}  ${String(total.files).padStart(5)}  ${String(total.replacements).padStart(12)}`)
if (errors) {
  console.error(`${errors} file(s) could not be processed.`)
  process.exit(1)
}
