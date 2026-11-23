#!/usr/bin/env node
// Builds the Hew modeler (the hew/ submodule) and copies it to resources/web/hew, where the
// Hew tab serves it from (src/slic3r/GUI/HewPanel.cpp, docs/HLSD/hew-tab.md).
//
// Prerequisites: the Rust toolchain named in hew/rust-toolchain.toml (with its wasm32 target),
// wasm-pack, Node and pnpm, plus `pnpm install` run once in hew/app.
//
// hew/shells/web has its own build script, but it is not used here: it relies on POSIX
// `rm` and `cp`, and this one has to run on Windows too.
//
// Usage: node scripts/build_hew.mjs

import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const appDir = path.join(repo, 'hew', 'app')
const distDir = path.join(appDir, 'dist')
const outDir = path.join(repo, 'resources', 'web', 'hew')

// shell: true so pnpm.cmd resolves on Windows.
const build = spawnSync('pnpm', ['--dir', `"${appDir}"`, 'build'], { stdio: 'inherit', shell: true })
if (build.error) {
  console.error(`Could not run pnpm: ${build.error.message}`)
  process.exit(1)
}
if (build.status !== 0) {
  console.error(`The Hew build failed (exit code ${build.status}).`)
  process.exit(build.status ?? 1)
}

fs.rmSync(outDir, { recursive: true, force: true })
fs.cpSync(distDir, outDir, { recursive: true })

// The host serves the bundle itself; a PWA service worker would keep serving an old bundle
// after the app is updated.
for (const name of fs.readdirSync(outDir)) {
  if (/^(sw|registerSW|workbox-.*)\.js(\.map)?$/.test(name)) {
    fs.rmSync(path.join(outDir, name), { force: true })
  }
}

let files = 0
const count = (dir) => {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.isDirectory()) count(path.join(dir, entry.name))
    else files++
  }
}
count(outDir)
console.log(`Hew bundle copied to ${outDir} (${files} files).`)
