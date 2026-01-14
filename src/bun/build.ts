// Copyright (c) 2024 Jeju Network
// Build script for bundling Bun compatibility TypeScript into JavaScript
//
// Output:
// - dist/bun/bun.js           (module)
// - dist/bun/sqlite.js        (module)
// - dist/bun/test.js          (module)
// - dist/bun/ffi.js           (module)
// - dist/bun/internal/*.js    (helpers)
// - dist/bun/bun-bundle.js    (single-file bundle for worker injection)

import { existsSync, mkdirSync } from 'node:fs'
import path from 'node:path'
import { type BuildOptions, build } from 'esbuild'

const __dirname = path.dirname(new URL(import.meta.url).pathname)
const outDir = path.join(__dirname, '../../dist/bun')

if (!existsSync(outDir)) {
  mkdirSync(outDir, { recursive: true })
}

const base: BuildOptions = {
  bundle: true,
  format: 'esm',
  target: 'esnext',
  platform: 'neutral', // workerd, not node
  minify: false,
  sourcemap: false,
  treeShaking: true,
}

async function buildModules() {
  console.log('Building Bun compatibility modules...')

  await build({
    ...base,
    entryPoints: [path.join(__dirname, 'bun.ts')],
    outfile: path.join(outDir, 'bun.js'),
    external: ['bun-internal:*', 'node:*'],
  })
  console.log('  - bun.js')

  await build({
    ...base,
    entryPoints: [path.join(__dirname, 'sqlite.ts')],
    outfile: path.join(outDir, 'sqlite.js'),
    external: ['bun-internal:*'],
  })
  console.log('  - sqlite.js')

  await build({
    ...base,
    entryPoints: [path.join(__dirname, 'test.ts')],
    outfile: path.join(outDir, 'test.js'),
    external: ['bun-internal:*'],
  })
  console.log('  - test.js')

  await build({
    ...base,
    entryPoints: [path.join(__dirname, 'ffi.ts')],
    outfile: path.join(outDir, 'ffi.js'),
    external: ['bun-internal:*'],
  })
  console.log('  - ffi.js')

  const internalDir = path.join(outDir, 'internal')
  if (!existsSync(internalDir)) {
    mkdirSync(internalDir, { recursive: true })
  }

  await build({
    ...base,
    entryPoints: [path.join(__dirname, 'internal/errors.ts')],
    outfile: path.join(internalDir, 'errors.js'),
  })
  console.log('  - internal/errors.js')

  await build({
    ...base,
    entryPoints: [path.join(__dirname, 'internal/types.ts')],
    outfile: path.join(internalDir, 'types.js'),
  })
  console.log('  - internal/types.js')

  console.log('\nBuild complete.')
  console.log(`Output directory: ${outDir}`)
}

async function buildWorkerBundle() {
  console.log('\nBuilding standalone worker bundle (bun-bundle.js)...')

  // Standalone bundle inlines internal modules. Only node:* is external (provided by nodejs_compat).
  await build({
    ...base,
    entryPoints: [path.join(__dirname, 'bun.ts')],
    outfile: path.join(outDir, 'bun-bundle.js'),
    external: ['node:*'],
    alias: {
      'bun-internal:errors': path.join(__dirname, 'internal/errors.ts'),
      'bun-internal:types': path.join(__dirname, 'internal/types.ts'),
    },
  })

  console.log(`  - bun-bundle.js (${path.join(outDir, 'bun-bundle.js')})`)
}

await buildModules()
await buildWorkerBundle()
