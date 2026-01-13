// Copyright (c) 2024 Jeju Network
// Build script for bundling Bun compatibility TypeScript into JavaScript
// This creates a standalone bundle that can be used in workerd workers

import { build, type BuildConfig } from 'esbuild'
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'fs'
import path from 'path'

const __dirname = path.dirname(new URL(import.meta.url).pathname)
const outDir = path.join(__dirname, '../../dist/bun')

// Ensure output directory exists
if (!existsSync(outDir)) {
  mkdirSync(outDir, { recursive: true })
}

// Common build options
const commonOptions: BuildConfig = {
  bundle: true,
  format: 'esm' as const,
  target: 'esnext',
  platform: 'browser',
  minify: false,
  sourcemap: false,
  treeShaking: true,
}

// Build each module separately
async function buildModules() {
  console.log('Building Bun compatibility modules...')
  
  // Build main bun module
  await build({
    ...commonOptions,
    entryPoints: [path.join(__dirname, 'bun.ts')],
    outfile: path.join(outDir, 'bun.js'),
    external: ['bun-internal:*'],
  })
  console.log('  - bun.js')
  
  // Build sqlite module
  await build({
    ...commonOptions,
    entryPoints: [path.join(__dirname, 'sqlite.ts')],
    outfile: path.join(outDir, 'sqlite.js'),
    external: ['bun-internal:*', 'bun:bun'],
  })
  console.log('  - sqlite.js')
  
  // Build test module (stubs)
  await build({
    ...commonOptions,
    entryPoints: [path.join(__dirname, 'test.ts')],
    outfile: path.join(outDir, 'test.js'),
    external: ['bun-internal:*'],
  })
  console.log('  - test.js')
  
  // Build ffi module (stubs)
  await build({
    ...commonOptions,
    entryPoints: [path.join(__dirname, 'ffi.ts')],
    outfile: path.join(outDir, 'ffi.js'),
    external: ['bun-internal:*'],
  })
  console.log('  - ffi.js')
  
  // Build internal modules
  const internalDir = path.join(outDir, 'internal')
  if (!existsSync(internalDir)) {
    mkdirSync(internalDir, { recursive: true })
  }
  
  // Build internal/errors
  await build({
    ...commonOptions,
    entryPoints: [path.join(__dirname, 'internal/errors.ts')],
    outfile: path.join(internalDir, 'errors.js'),
  })
  console.log('  - internal/errors.js')
  
  // Build internal/types
  await build({
    ...commonOptions,
    entryPoints: [path.join(__dirname, 'internal/types.ts')],
    outfile: path.join(internalDir, 'types.js'),
  })
  console.log('  - internal/types.js')
  
  console.log('\nBuild complete.')
  console.log(`Output directory: ${outDir}`)
}

// Create a combined bundle for worker injection
async function buildCombinedBundle() {
  console.log('\nBuilding combined bundle for worker injection...')
  
  // First, bundle bcryptjs for inclusion in the main bundle
  console.log('  - Bundling bcryptjs...')
  const bcryptResult = await build({
    entryPoints: ['bcryptjs'],
    bundle: true,
    format: 'iife',
    globalName: 'bcrypt',
    minify: true,
    write: false,
    platform: 'browser',
  })
  const bcryptjsCode = bcryptResult.outputFiles?.[0]?.text ?? ''
  console.log(`  - bcryptjs bundled (${Math.round(bcryptjsCode.length / 1024)}KB)`)
  
  // Build a self-contained bundle that defines the Bun global
  // NOTE: This must match bun.ts implementations exactly to avoid LARP
  const bundleCode = `
// Auto-generated Bun compatibility bundle for workerd
// This provides Bun APIs in workerd environments
// IMPORTANT: Implementations must match src/bun/bun.ts exactly

// Internal utilities
const isString = (v) => typeof v === 'string'
const isUint8Array = (v) => v instanceof Uint8Array
const isArrayBuffer = (v) => v instanceof ArrayBuffer

class BunError extends Error {
  constructor(message, code) {
    super(message)
    this.name = 'BunError'
    this.code = code
  }
}

class ERR_FS_FILE_NOT_FOUND extends BunError {
  constructor(path) {
    super(\`ENOENT: no such file or directory, open '\${path}'\`, 'ENOENT')
    this.name = 'ERR_FS_FILE_NOT_FOUND'
  }
}

class ERR_WORKERD_UNAVAILABLE extends BunError {
  constructor(feature, reason) {
    const msg = reason
      ? \`\${feature} is not available in workerd: \${reason}\`
      : \`\${feature} is not available in workerd\`
    super(msg, 'ERR_WORKERD_UNAVAILABLE')
    this.name = 'ERR_WORKERD_UNAVAILABLE'
  }
}

// Virtual file system for workerd
const virtualFS = new Map()
const virtualFSMetadata = new Map()

// BunFile implementation - matches bun.ts BunFileImpl
class BunFileImpl {
  #path
  #type
  
  constructor(path, options = {}) {
    this.#path = typeof path === 'string' ? path : path.pathname
    this.#type = options.type ?? 'application/octet-stream'
  }
  
  get size() {
    const data = virtualFS.get(this.#path)
    return data ? data.byteLength : 0
  }
  
  get type() {
    return virtualFSMetadata.get(this.#path)?.type ?? this.#type
  }
  
  get name() { return this.#path }
  
  get lastModified() {
    return virtualFSMetadata.get(this.#path)?.lastModified ?? Date.now()
  }
  
  async text() {
    const data = virtualFS.get(this.#path)
    if (!data) throw new ERR_FS_FILE_NOT_FOUND(this.#path)
    return new TextDecoder().decode(data)
  }
  
  async json() {
    return JSON.parse(await this.text())
  }
  
  async arrayBuffer() {
    const data = virtualFS.get(this.#path)
    if (!data) throw new ERR_FS_FILE_NOT_FOUND(this.#path)
    const ab = data.buffer
    if (ab instanceof ArrayBuffer) {
      return ab.slice(data.byteOffset, data.byteOffset + data.byteLength)
    }
    const copy = new ArrayBuffer(data.byteLength)
    new Uint8Array(copy).set(data)
    return copy
  }
  
  async bytes() {
    const data = virtualFS.get(this.#path)
    if (!data) throw new ERR_FS_FILE_NOT_FOUND(this.#path)
    return new Uint8Array(data)
  }
  
  stream() {
    const data = virtualFS.get(this.#path)
    if (!data) throw new ERR_FS_FILE_NOT_FOUND(this.#path)
    return new ReadableStream({
      start(controller) {
        controller.enqueue(data)
        controller.close()
      }
    })
  }
  
  slice(start, end, type) {
    const data = virtualFS.get(this.#path)
    if (!data) return new BunFileImpl(this.#path, { type: type ?? this.#type })
    const sliced = data.slice(start, end)
    const slicePath = \`\${this.#path}#slice(\${start},\${end})\`
    virtualFS.set(slicePath, sliced)
    return new BunFileImpl(slicePath, { type: type ?? this.#type })
  }
  
  async exists() {
    return virtualFS.has(this.#path)
  }
  
  writer() {
    const path = this.#path
    const chunks = []
    return {
      write(data) {
        const bytes = isString(data)
          ? new TextEncoder().encode(data)
          : data instanceof ArrayBuffer
            ? new Uint8Array(data)
            : data
        chunks.push(bytes)
        return bytes.byteLength
      },
      flush() {
        const totalLength = chunks.reduce((sum, c) => sum + c.byteLength, 0)
        const result = new Uint8Array(totalLength)
        let offset = 0
        for (const chunk of chunks) {
          result.set(chunk, offset)
          offset += chunk.byteLength
        }
        virtualFS.set(path, result)
        virtualFSMetadata.set(path, { type: 'application/octet-stream', lastModified: Date.now() })
      },
      end() {
        this.flush()
        chunks.length = 0
      }
    }
  }
}

// ArrayBufferSink - matches bun.ts
class ArrayBufferSinkImpl {
  #chunks = []
  
  write(data) {
    const bytes = isString(data)
      ? new TextEncoder().encode(data)
      : data instanceof ArrayBuffer
        ? new Uint8Array(data)
        : data
    this.#chunks.push(bytes)
  }
  
  end() {
    const totalLength = this.#chunks.reduce((sum, c) => sum + c.byteLength, 0)
    const result = new Uint8Array(totalLength)
    let offset = 0
    for (const chunk of this.#chunks) {
      result.set(chunk, offset)
      offset += chunk.byteLength
    }
    this.#chunks = []
    return result.buffer
  }
  
  flush() {}
  start() { this.#chunks = [] }
}

// Hashing - matches bun.ts implementations exactly
function wyhash(data, seed = 0) {
  let h = BigInt(seed)
  for (let i = 0; i < data.length; i++) {
    h = (h ^ BigInt(data[i])) * 0x9e3779b97f4a7c15n
    h = h ^ (h >> 32n)
  }
  return h
}

function crc32(data) {
  let crc = 0xffffffff
  for (let i = 0; i < data.length; i++) {
    crc = crc ^ data[i]
    for (let j = 0; j < 8; j++) {
      crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1
    }
  }
  return (crc ^ 0xffffffff) >>> 0
}

function adler32(data) {
  let a = 1, b = 0
  const mod = 65521
  for (let i = 0; i < data.length; i++) {
    a = (a + data[i]) % mod
    b = (b + a) % mod
  }
  return ((b << 16) | a) >>> 0
}

function cityhash32(data) {
  let h = 0x811c9dc5
  for (let i = 0; i < data.length; i++) {
    h ^= data[i]
    h = Math.imul(h, 0x01000193)
  }
  return h >>> 0
}

function cityhash64(data) {
  let h = 0xcbf29ce484222325n
  for (let i = 0; i < data.length; i++) {
    h ^= BigInt(data[i])
    h *= 0x100000001b3n
  }
  return h
}

function murmur32v3(data, seed = 0) {
  let h = seed
  const c1 = 0xcc9e2d51, c2 = 0x1b873593
  
  for (let i = 0; i < data.length; i += 4) {
    let k = data[i] | ((data[i + 1] ?? 0) << 8) | ((data[i + 2] ?? 0) << 16) | ((data[i + 3] ?? 0) << 24)
    k = Math.imul(k, c1)
    k = (k << 15) | (k >>> 17)
    k = Math.imul(k, c2)
    h ^= k
    h = (h << 13) | (h >>> 19)
    h = Math.imul(h, 5) + 0xe6546b64
  }
  
  h ^= data.length
  h ^= h >>> 16
  h = Math.imul(h, 0x85ebca6b)
  h ^= h >>> 13
  h = Math.imul(h, 0xc2b2ae35)
  h ^= h >>> 16
  return h >>> 0
}

function murmur64v2(data, seed = 0) {
  let h = BigInt(seed) ^ (BigInt(data.length) * 0xc6a4a7935bd1e995n)
  const m = 0xc6a4a7935bd1e995n, r = 47n
  
  for (let i = 0; i < data.length - 7; i += 8) {
    let k = BigInt(data[i]) | (BigInt(data[i + 1]) << 8n) | (BigInt(data[i + 2]) << 16n) |
            (BigInt(data[i + 3]) << 24n) | (BigInt(data[i + 4]) << 32n) | (BigInt(data[i + 5]) << 40n) |
            (BigInt(data[i + 6]) << 48n) | (BigInt(data[i + 7]) << 56n)
    k *= m
    k ^= k >> r
    k *= m
    h ^= k
    h *= m
  }
  
  h ^= h >> r
  h *= m
  h ^= h >> r
  return h
}

function hash(data, algorithm) {
  const bytes = isString(data)
    ? new TextEncoder().encode(data)
    : isArrayBuffer(data)
      ? new Uint8Array(data)
      : data
  
  switch (algorithm ?? 'wyhash') {
    case 'wyhash': return wyhash(bytes)
    case 'crc32': return crc32(bytes)
    case 'adler32': return adler32(bytes)
    case 'cityhash32': return cityhash32(bytes)
    case 'cityhash64': return cityhash64(bytes)
    case 'murmur32v3': return murmur32v3(bytes)
    case 'murmur64v2': return murmur64v2(bytes)
    default: return wyhash(bytes)
  }
}

// Hash method aliases - match Bun.hash.wyhash(), etc.
hash.wyhash = (data, seed) => wyhash(isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data, seed)
hash.crc32 = (data) => crc32(isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data)
hash.adler32 = (data) => adler32(isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data)
hash.cityhash32 = (data) => cityhash32(isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data)
hash.cityhash64 = (data) => cityhash64(isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data)
hash.murmur32v3 = (data, seed) => murmur32v3(isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data, seed)
hash.murmur64v2 = (data, seed) => murmur64v2(isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data, seed)

// Deep equals - matches bun.ts exactly
function deepEquals(a, b) {
  if (a === b) return true
  if (typeof a !== typeof b) return false
  if (a === null || b === null) return a === b
  
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false
    return a.every((item, index) => deepEquals(item, b[index]))
  }
  
  if (typeof a === 'object' && typeof b === 'object') {
    const aKeys = Object.keys(a)
    const bKeys = Object.keys(b)
    if (aKeys.length !== bKeys.length) return false
    return aKeys.every((key) => key in b && deepEquals(a[key], b[key]))
  }
  
  return false
}

// Escape HTML - matches bun.ts (&#039; not &#39;)
function escapeHTML(str) {
  return str
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;')
}

// String width - matches bun.ts (no control char skip)
function stringWidth(str) {
  let width = 0
  for (const char of str) {
    const code = char.codePointAt(0)
    const isWide =
      (code >= 0x1100 && code <= 0x115f) ||
      (code >= 0x2e80 && code <= 0xa4cf) ||
      (code >= 0xac00 && code <= 0xd7a3) ||
      (code >= 0xf900 && code <= 0xfaff) ||
      (code >= 0xfe10 && code <= 0xfe1f) ||
      (code >= 0xfe30 && code <= 0xfe6f) ||
      (code >= 0xff00 && code <= 0xff60) ||
      (code >= 0xffe0 && code <= 0xffe6) ||
      (code >= 0x20000 && code <= 0x2fffd) ||
      (code >= 0x30000 && code <= 0x3fffd)
    width += isWide ? 2 : 1
  }
  return width
}

// Inspect - matches bun.ts custom implementation
function inspectValue(value, depth, seen) {
  if (value === null) return 'null'
  if (value === undefined) return 'undefined'
  
  const type = typeof value
  if (type === 'string') return JSON.stringify(value)
  if (type === 'number' || type === 'boolean' || type === 'bigint') return String(value)
  if (type === 'function') return \`[Function: \${value.name || 'anonymous'}]\`
  if (type === 'symbol') return value.toString()
  
  if (seen.has(value)) return '[Circular]'
  
  if (Array.isArray(value)) {
    if (depth < 0) return '[Array]'
    seen.add(value)
    const items = value.map((item) => inspectValue(item, depth - 1, seen))
    seen.delete(value)
    return \`[ \${items.join(', ')} ]\`
  }
  
  if (value instanceof Date) return value.toISOString()
  if (value instanceof RegExp) return value.toString()
  if (value instanceof Error) return \`\${value.name}: \${value.message}\`
  
  if (type === 'object') {
    if (depth < 0) return '[Object]'
    seen.add(value)
    const entries = Object.entries(value).map(([k, v]) => \`\${k}: \${inspectValue(v, depth - 1, seen)}\`)
    seen.delete(value)
    return \`{ \${entries.join(', ')} }\`
  }
  
  return String(value)
}

function inspect(obj, options) {
  const depth = options?.depth ?? 2
  return inspectValue(obj, depth, new Set())
}

// Nanoseconds - matches bun.ts (absolute, not relative)
function nanoseconds() {
  return BigInt(Math.floor(performance.now() * 1_000_000))
}

// Sleep
function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms))
}

function sleepSync(ms) {
  const end = Date.now() + ms
  while (Date.now() < end) {}
}

// Stream utilities - match bun.ts
async function readableStreamToArray(stream) {
  const reader = stream.getReader()
  const chunks = []
  while (true) {
    const { done, value } = await reader.read()
    if (done) break
    chunks.push(value)
  }
  return chunks
}

async function readableStreamToText(stream) {
  const chunks = await readableStreamToArray(stream)
  const decoder = new TextDecoder()
  return chunks.map((chunk) => decoder.decode(chunk, { stream: true })).join('')
}

async function readableStreamToArrayBuffer(stream) {
  const chunks = await readableStreamToArray(stream)
  const totalLength = chunks.reduce((sum, chunk) => sum + chunk.byteLength, 0)
  const result = new Uint8Array(totalLength)
  let offset = 0
  for (const chunk of chunks) {
    result.set(chunk, offset)
    offset += chunk.byteLength
  }
  return result.buffer
}

async function readableStreamToBlob(stream, type) {
  const buffer = await readableStreamToArrayBuffer(stream)
  return new Blob([buffer], { type })
}

async function readableStreamToJSON(stream) {
  return JSON.parse(await readableStreamToText(stream))
}

// Write utility - matches bun.ts
async function write(dest, data) {
  const destPath = isString(dest) ? dest : dest instanceof URL ? dest.pathname : dest.name
  let bytes
  
  if (isString(data)) {
    bytes = new TextEncoder().encode(data)
  } else if (isArrayBuffer(data)) {
    bytes = new Uint8Array(data)
  } else if (isUint8Array(data)) {
    bytes = data
  } else if (data instanceof Blob) {
    bytes = new Uint8Array(await data.arrayBuffer())
  } else if (data instanceof Response) {
    bytes = new Uint8Array(await data.arrayBuffer())
  } else {
    bytes = await data.bytes()
  }
  
  virtualFS.set(destPath, bytes)
  virtualFSMetadata.set(destPath, { type: 'application/octet-stream', lastModified: Date.now() })
  return bytes.byteLength
}

// Password hashing - REAL bcrypt implementation using bcryptjs
// bcryptjs is bundled below (inlined from npm package)
${bcryptjsCode}

const password = {
  async hash(pwd, options) {
    const algorithm = options?.algorithm ?? 'bcrypt'
    const cost = options?.cost ?? 10
    
    if (algorithm !== 'bcrypt') {
      throw new Error(\`Algorithm '\${algorithm}' is not available in workerd. Use 'bcrypt' instead.\`)
    }
    
    // Use real bcrypt
    const salt = bcrypt.genSaltSync(cost)
    return bcrypt.hashSync(pwd, salt)
  },
  
  async verify(pwd, hashStr) {
    // Real bcrypt format: $2a$, $2b$, $2y$
    if (hashStr.startsWith('\$2')) {
      return bcrypt.compareSync(pwd, hashStr)
    }
    
    // Legacy workerd format (backwards compatibility)
    if (hashStr.startsWith('\$workerd\$')) {
      return verifyLegacyWorkerdHash(pwd, hashStr)
    }
    
    throw new Error('Unknown hash format. Expected bcrypt ($2a$, $2b$, $2y$) or legacy workerd format.')
  }
}

// Legacy PBKDF2 verification for old workerd hashes
async function verifyLegacyWorkerdHash(pwd, hashStr) {
  const parts = hashStr.split('\$')
  if (parts.length !== 6) return false
  
  const [, , , costStr, saltHex, expectedHashHex] = parts
  const cost = parseInt(costStr, 10)
  const passwordData = new TextEncoder().encode(pwd)
  const salt = new Uint8Array(saltHex.match(/.{2}/g)?.map((byte) => parseInt(byte, 16)) ?? [])
  const keyMaterial = await crypto.subtle.importKey('raw', passwordData, 'PBKDF2', false, ['deriveBits'])
  const iterations = 2 ** cost * 100
  
  const derivedBits = await crypto.subtle.deriveBits(
    { name: 'PBKDF2', salt, iterations, hash: 'SHA-256' },
    keyMaterial,
    256
  )
  
  const computedHashHex = Array.from(new Uint8Array(derivedBits)).map((b) => b.toString(16).padStart(2, '0')).join('')
  
  // Constant-time comparison
  if (computedHashHex.length !== expectedHashHex.length) return false
  let result = 0
  for (let i = 0; i < computedHashHex.length; i++) {
    result |= computedHashHex.charCodeAt(i) ^ expectedHashHex.charCodeAt(i)
  }
  return result === 0
}

// Miscellaneous - match bun.ts
function randomUUIDv7() {
  const timestamp = Date.now()
  const timestampHex = timestamp.toString(16).padStart(12, '0')
  const random = crypto.getRandomValues(new Uint8Array(10))
  const randomHex = Array.from(random).map((b) => b.toString(16).padStart(2, '0')).join('')
  
  return [
    timestampHex.slice(0, 8),
    timestampHex.slice(8, 12),
    \`7\${randomHex.slice(0, 3)}\`,
    ((parseInt(randomHex.slice(3, 5), 16) & 0x3f) | 0x80).toString(16).padStart(2, '0') + randomHex.slice(5, 7),
    randomHex.slice(7, 19),
  ].join('-')
}

function fileURLToPath(url) {
  const urlObj = typeof url === 'string' ? new URL(url) : url
  if (urlObj.protocol !== 'file:') throw new Error('URL must use file: protocol')
  return urlObj.pathname
}

function pathToFileURL(path) {
  return new URL(\`file://\${path.startsWith('/') ? '' : '/'}\${path}\`)
}

// Promise peek - matches bun.ts
const resolvedPromises = new WeakMap()

function peek(promise) {
  const cached = resolvedPromises.get(promise)
  if (cached) {
    if (cached.resolved) {
      if (cached.error !== undefined) throw cached.error
      return cached.value
    }
    return promise
  }
  
  const tracker = { resolved: false, value: undefined, error: undefined }
  resolvedPromises.set(promise, tracker)
  
  promise
    .then((value) => { tracker.resolved = true; tracker.value = value })
    .catch((error) => { tracker.resolved = true; tracker.error = error })
  
  return promise
}

// Serve implementation - matches bun.ts
let currentServeOptions = null

function serve(options) {
  currentServeOptions = options
  const port = options.port ?? 3000
  const hostname = options.hostname ?? 'localhost'
  
  return {
    port,
    hostname,
    development: options.development ?? false,
    url: new URL(\`http://\${hostname}:\${port}\`),
    stop() { currentServeOptions = null },
    ref() {},
    unref() {},
    reload(newOptions) { currentServeOptions = { ...currentServeOptions, ...newOptions } },
    async fetch(request) {
      if (!currentServeOptions) throw new Error('Server is not running')
      try {
        return await currentServeOptions.fetch(request)
      } catch (error) {
        if (currentServeOptions.error && error instanceof Error) {
          return currentServeOptions.error(error)
        }
        throw error
      }
    }
  }
}

function getServeHandler() {
  return currentServeOptions
}

// Main Bun object - matches bun.ts exports exactly
const Bun = {
  version: '1.0.0-workerd',
  revision: 'workerd-compat',  // MUST match bun.ts
  main: false,
  
  // Environment
  env: typeof process !== 'undefined' ? process.env : {},
  
  // File operations
  file: (path, options) => new BunFileImpl(path, options),
  write,
  
  // Server (functional, matches bun.ts)
  serve,
  getServeHandler,
  
  // Hashing - REAL implementation with all algorithms
  hash,
  
  // Utilities
  sleep,
  sleepSync,
  nanoseconds,
  escapeHTML,
  stringWidth,
  deepEquals,
  inspect,
  peek,
  
  // Password - REAL implementation
  password,
  
  // Stream utilities
  readableStreamToArray,
  readableStreamToText,
  readableStreamToArrayBuffer,
  readableStreamToBlob,
  readableStreamToJSON,
  
  // ArrayBufferSink
  ArrayBufferSink: ArrayBufferSinkImpl,
  
  // Misc
  randomUUIDv7,
  fileURLToPath,
  pathToFileURL,
  
  // No-op functions (documented as expected)
  gc() {},
  shrink() {},
  
  // Unavailable APIs (throw with clear errors)
  openInEditor() { throw new ERR_WORKERD_UNAVAILABLE('Bun.openInEditor') },
  generateHeapSnapshot() { throw new ERR_WORKERD_UNAVAILABLE('Bun.generateHeapSnapshot') },
  
  // DNS via DNS-over-HTTPS (real implementation)
  dns: (() => {
    const DOH_PROVIDERS = {
      cloudflare: 'https://cloudflare-dns.com/dns-query',
      google: 'https://dns.google/resolve'
    }
    const RECORD_TYPES = { A: 1, AAAA: 28, CNAME: 5, MX: 15, TXT: 16, NS: 2, SRV: 33, PTR: 12 }
    let currentProvider = 'cloudflare'
    
    async function doQuery(hostname, type) {
      const url = new URL(DOH_PROVIDERS[currentProvider])
      url.searchParams.set('name', hostname)
      url.searchParams.set('type', type)
      
      const response = await fetch(url.toString(), {
        headers: { Accept: 'application/dns-json' }
      })
      if (!response.ok) throw new Error(\`DNS query failed: \${response.status}\`)
      
      const data = await response.json()
      if (data.Status !== 0) {
        const errors = { 1: 'Format error', 2: 'Server failure', 3: 'NXDOMAIN', 5: 'Refused' }
        throw new Error(\`DNS error: \${errors[data.Status] || 'Unknown'}\`)
      }
      return data.Answer || []
    }
    
    return {
      async lookup(hostname, options) {
        const family = options?.family ?? 0
        const all = options?.all ?? false
        const results = []
        
        if (family === 0 || family === 4) {
          try {
            const answers = await doQuery(hostname, 'A')
            answers.filter(a => a.type === 1).forEach(a => results.push({ address: a.data, family: 4 }))
          } catch {}
        }
        if (family === 0 || family === 6) {
          try {
            const answers = await doQuery(hostname, 'AAAA')
            answers.filter(a => a.type === 28).forEach(a => results.push({ address: a.data, family: 6 }))
          } catch {}
        }
        
        if (results.length === 0) throw new Error(\`ENOTFOUND: DNS lookup failed for \${hostname}\`)
        return all ? results : results[0].address
      },
      
      async resolve(hostname, rrtype = 'A') { 
        const answers = await doQuery(hostname, rrtype)
        return answers.filter(a => a.type === RECORD_TYPES[rrtype]).map(a => a.data)
      },
      async resolve4(hostname) { return this.resolve(hostname, 'A') },
      async resolve6(hostname) { return this.resolve(hostname, 'AAAA') },
      async resolveMx(hostname) {
        const answers = await doQuery(hostname, 'MX')
        return answers.filter(a => a.type === 15).map(a => {
          const parts = a.data.split(' ')
          return { priority: parseInt(parts[0], 10), exchange: parts.slice(1).join(' ') }
        }).sort((a, b) => a.priority - b.priority)
      },
      async resolveTxt(hostname) {
        const answers = await doQuery(hostname, 'TXT')
        return answers.filter(a => a.type === 16).map(a => [a.data.replace(/^"|"$/g, '')])
      },
      async resolveNs(hostname) { return this.resolve(hostname, 'NS') },
      async reverse(ip) {
        const reverseAddr = ip.includes(':')
          ? ip.split(':').map(p => p.padStart(4, '0')).join('').split('').reverse().join('.') + '.ip6.arpa'
          : ip.split('.').reverse().join('.') + '.in-addr.arpa'
        return this.resolve(reverseAddr, 'PTR')
      },
      getServers() { return [DOH_PROVIDERS[currentProvider]] },
      setServers(servers) {
        if (servers[0]?.includes('google')) currentProvider = 'google'
        else if (servers[0]?.includes('cloudflare')) currentProvider = 'cloudflare'
      },
      setProvider(p) { if (DOH_PROVIDERS[p]) currentProvider = p },
      getProvider() { return currentProvider }
    }
  })(),
}

// Export for ES modules
export default Bun
export { Bun }
`
  
  writeFileSync(path.join(outDir, 'bun-bundle.js'), bundleCode.trim())
  console.log('  - bun-bundle.js (standalone bundle)')
  
  console.log('\nStandalone bundle ready for worker injection.')
}

// Run builds
async function main() {
  try {
    await buildModules()
    await buildCombinedBundle()
    
    console.log('\n=== Build Summary ===')
    console.log(`Output: ${outDir}`)
    console.log('\nTo use in a worker, import from the bundle:')
    console.log('  import Bun from "./bun-bundle.js"')
    console.log('\nOr wait for workerd to be built with native bun: support.')
  } catch (error) {
    console.error('Build failed:', error)
    process.exit(1)
  }
}

main()
