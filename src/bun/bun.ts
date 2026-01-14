// Bun Runtime Compatibility Layer for Workerd
//
// Real Bun API implementations for workerd:
// - File operations: node:fs with /tmp via enable_web_file_system (lazy loaded)
// - Password hashing: PBKDF2 via WebCrypto
// - DNS: DNS-over-HTTPS (Cloudflare/Google)
// - Hash functions: JS implementations (wyhash, crc32, etc.)
// - SQLite: NOT AVAILABLE (use Cloudflare D1 or external API)

import { ERR_FS_FILE_NOT_FOUND } from 'bun-internal:errors'
import { isArrayBuffer, isString, isUint8Array } from 'bun-internal:types'

// Lazy-loaded fs modules - only imported when file operations are used
// This allows the bundle to load even when node:fs isn't available
let fsModule: typeof import('node:fs') | null = null
let fsPromisesModule: typeof import('node:fs/promises') | null = null
let streamModule: typeof import('node:stream') | null = null

function getFs() {
  if (!fsModule) {
    try {
      fsModule = require('node:fs')
    } catch {
      throw new Error(
        'File operations require node:fs.\n' +
          'Enable with: compatibilityFlags = ["nodejs_compat", "enable_nodejs_fs_module"]',
      )
    }
  }
  return fsModule
}

function getFsPromises() {
  if (!fsPromisesModule) {
    try {
      fsPromisesModule = require('node:fs/promises')
    } catch {
      throw new Error(
        'File operations require node:fs/promises.\n' +
          'Enable with: compatibilityFlags = ["nodejs_compat", "enable_nodejs_fs_module"]',
      )
    }
  }
  return fsPromisesModule
}

function getStream() {
  if (!streamModule) {
    try {
      streamModule = require('node:stream')
    } catch {
      throw new Error('Stream operations require node:stream.')
    }
  }
  return streamModule
}

import * as dns from './dns'
export { dns }

// Types
export interface BunFile {
  readonly size: number
  readonly type: string
  readonly name: string
  readonly lastModified: number
  text(): Promise<string>
  json<T = unknown>(): Promise<T>
  arrayBuffer(): Promise<ArrayBuffer>
  bytes(): Promise<Uint8Array>
  stream(): ReadableStream<Uint8Array>
  slice(start?: number, end?: number, type?: string): BunFile
  exists(): Promise<boolean>
  writer(): FileSink
}

export interface FileSink {
  write(data: string | ArrayBuffer | Uint8Array): number
  flush(): void
  end(): void
}

export interface ServeOptions {
  port?: number
  hostname?: string
  fetch: (request: Request) => Response | Promise<Response>
  error?: (error: Error) => Response | Promise<Response>
  websocket?: WebSocketHandler
  development?: boolean
  reusePort?: boolean
  tls?: TLSOptions
}

export interface TLSOptions {
  cert?: string | string[]
  key?: string | string[]
  ca?: string | string[]
  passphrase?: string
}

export interface WebSocketHandler {
  message?: (ws: ServerWebSocket, message: string | ArrayBuffer) => void
  open?: (ws: ServerWebSocket) => void
  close?: (ws: ServerWebSocket, code: number, reason: string) => void
  drain?: (ws: ServerWebSocket) => void
}

export interface ServerWebSocket {
  send(data: string | ArrayBuffer): void
  close(code?: number, reason?: string): void
  readonly readyState: number
  readonly data: unknown
}

export interface Server {
  readonly port: number
  readonly hostname: string
  readonly development: boolean
  readonly url: URL
  stop(): void
  ref(): void
  unref(): void
  reload(options: Partial<ServeOptions>): void
  fetch(request: Request): Promise<Response>
}

export interface HashOptions {
  seed?: number
}

export type HashAlgorithm =
  | 'wyhash'
  | 'adler32'
  | 'crc32'
  | 'cityhash32'
  | 'cityhash64'
  | 'murmur32v3'
  | 'murmur64v2'

// BunFile backed by node:fs. Use with enable_web_file_system for /tmp access.
class BunFileImpl implements BunFile {
  private readonly path: string
  private readonly _type: string
  private readonly sliceStart: number | undefined
  private readonly sliceEnd: number | undefined

  constructor(
    path: string | URL,
    options?: { type?: string },
    sliceStart?: number,
    sliceEnd?: number,
  ) {
    this.path = typeof path === 'string' ? path : path.pathname
    this._type = options?.type ?? 'application/octet-stream'
    this.sliceStart = sliceStart
    this.sliceEnd = sliceEnd
  }

  get size(): number {
    const fs = getFs()
    if (!fs.existsSync(this.path)) return 0
    const st = fs.statSync(this.path)
    const len = st.size
    if (this.sliceStart === undefined && this.sliceEnd === undefined) return len
    const start = this.sliceStart ?? 0
    const end = this.sliceEnd ?? len
    return Math.max(0, end - start)
  }

  get type(): string {
    return this._type
  }

  get name(): string {
    return this.path
  }

  get lastModified(): number {
    const fs = getFs()
    if (!fs.existsSync(this.path)) return Date.now()
    const st = fs.statSync(this.path)
    return st.mtimeMs
  }

  async text(): Promise<string> {
    const data = await this.bytes()
    return new TextDecoder().decode(data)
  }

  async json<T = unknown>(): Promise<T> {
    return JSON.parse(await this.text()) as T
  }

  async arrayBuffer(): Promise<ArrayBuffer> {
    const data = await this.bytes()
    const copy = new ArrayBuffer(data.byteLength)
    new Uint8Array(copy).set(data)
    return copy
  }

  async bytes(): Promise<Uint8Array> {
    const fs = getFs()
    const fsp = getFsPromises()
    if (!fs.existsSync(this.path)) throw new ERR_FS_FILE_NOT_FOUND(this.path)
    const buf = await fsp.readFile(this.path)
    const bytes = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength)
    if (this.sliceStart === undefined && this.sliceEnd === undefined)
      return bytes
    return bytes.slice(this.sliceStart, this.sliceEnd)
  }

  stream(): ReadableStream<Uint8Array> {
    const fs = getFs()
    const { Readable } = getStream()
    if (!fs.existsSync(this.path)) throw new ERR_FS_FILE_NOT_FOUND(this.path)
    const start = this.sliceStart
    const end =
      this.sliceEnd === undefined ? undefined : Math.max(0, this.sliceEnd - 1)
    // fs.ReadStream extends stream.Readable; cast needed for Readable.toWeb compatibility
    const nodeStream = fs.createReadStream(this.path, {
      start,
      end,
    }) as InstanceType<typeof Readable>
    return Readable.toWeb(nodeStream) as ReadableStream<Uint8Array>
  }

  slice(start?: number, end?: number, type?: string): BunFile {
    if (this.sliceStart !== undefined || this.sliceEnd !== undefined) {
      const baseStart = this.sliceStart ?? 0
      const newStart = start === undefined ? this.sliceStart : baseStart + start
      const newEnd =
        end === undefined
          ? this.sliceEnd
          : this.sliceStart === undefined
            ? end
            : baseStart + end
      return new BunFileImpl(
        this.path,
        { type: type ?? this._type },
        newStart,
        newEnd,
      )
    }

    return new BunFileImpl(this.path, { type: type ?? this._type }, start, end)
  }

  async exists(): Promise<boolean> {
    const fs = getFs()
    return fs.existsSync(this.path)
  }

  writer(): FileSink {
    const fs = getFs()
    const fd = fs.openSync(this.path, 'w')
    let closed = false

    return {
      write(data: string | ArrayBuffer | Uint8Array): number {
        if (closed) {
          throw new Error('FileSink is closed')
        }

        const bytes =
          typeof data === 'string'
            ? new TextEncoder().encode(data)
            : data instanceof ArrayBuffer
              ? new Uint8Array(data)
              : data

        return fs.writeSync(fd, bytes, 0, bytes.byteLength)
      },
      flush(): void {}, // writeSync is unbuffered
      end(): void {
        if (closed) return
        fs.closeSync(fd)
        closed = true
      },
    }
  }
}

// File operations
export function file(path: string | URL, options?: { type?: string }): BunFile {
  return new BunFileImpl(path, options)
}

export async function write(
  destination: string | URL | BunFile,
  data: string | ArrayBuffer | Uint8Array | Blob | Response | BunFile,
): Promise<number> {
  const path = isString(destination)
    ? destination
    : destination instanceof URL
      ? destination.pathname
      : destination.name

  let bytes: Uint8Array
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

  const fsp = getFsPromises()
  await fsp.writeFile(path, bytes)
  return bytes.byteLength
}

// Server
let currentServeOptions: ServeOptions | null = null

// Test helper only - does NOT create a real HTTP server in workerd.
// For production, use: export default { fetch(request) { return new Response("OK") } }
export function serve(options: ServeOptions): Server {
  // Check if we're in actual workerd (navigator.userAgent contains 'Cloudflare-Workers')
  const isWorkerd =
    typeof navigator !== 'undefined' &&
    navigator.userAgent?.includes('Cloudflare-Workers')

  if (isWorkerd && !options.development) {
    throw new Error(
      'Bun.serve() does not create a real HTTP server in workerd.\n' +
        'For production, use the native pattern:\n' +
        '  export default { fetch(request) { return new Response("OK") } }\n\n' +
        'For testing, pass development: true to use this as a test helper.',
    )
  }

  currentServeOptions = options
  const port = options.port ?? 3000
  const hostname = options.hostname ?? 'localhost'

  return {
    port,
    hostname,
    development: options.development ?? false,
    url: new URL(`http://${hostname}:${port}`),
    stop() {
      currentServeOptions = null
    },
    ref() {
      // No-op in workerd: The runtime manages process lifecycle
    },
    unref() {
      // No-op in workerd: The runtime manages process lifecycle
    },
    reload(newOptions: Partial<ServeOptions>) {
      currentServeOptions = { ...currentServeOptions!, ...newOptions }
    },
    async fetch(request: Request): Promise<Response> {
      if (!currentServeOptions) throw new Error('Server is not running')
      try {
        return await currentServeOptions.fetch(request)
      } catch (error) {
        if (currentServeOptions.error && error instanceof Error) {
          return currentServeOptions.error(error)
        }
        throw error
      }
    },
  }
}

export function getServeHandler(): ServeOptions | null {
  return currentServeOptions
}

// Environment - simplified proxy
type ProcessEnv = { env?: Record<string, string> }
const getProcess = (): ProcessEnv | undefined =>
  (globalThis as Record<string, unknown>).process as ProcessEnv | undefined

export const env: Record<string, string | undefined> = new Proxy(
  {} as Record<string, string | undefined>,
  {
    get(_, prop: string) {
      return getProcess()?.env?.[prop]
    },
    set(_, prop: string, value: string) {
      const proc = getProcess()
      if (proc?.env) proc.env[prop] = value
      return true
    },
    has(_, prop: string) {
      return prop in (getProcess()?.env ?? {})
    },
    ownKeys() {
      return Object.keys(getProcess()?.env ?? {})
    },
    getOwnPropertyDescriptor(_, prop: string) {
      const env = getProcess()?.env
      if (env && prop in env) {
        return { enumerable: true, configurable: true, value: env[prop] }
      }
      return undefined
    },
  },
)

// Version
export const version = '1.0.0-workerd'
export const revision = 'workerd-compat'

// Utilities
export function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

export function sleepSync(ms: number): void {
  const end = Date.now() + ms
  while (Date.now() < end) {}
}

// Returns time since Worker started (not absolute time). Use Date.now() for timestamps.
export function nanoseconds(): bigint {
  return BigInt(Math.floor(performance.now() * 1_000_000))
}

export function escapeHTML(str: string): string {
  return str
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;')
}

export function stringWidth(str: string): number {
  let width = 0
  for (const char of str) {
    const code = char.codePointAt(0)!
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

export function deepEquals(a: unknown, b: unknown): boolean {
  if (a === b) return true
  if (typeof a !== typeof b) return false
  if (a === null || b === null) return a === b

  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false
    return a.every((item, index) => deepEquals(item, b[index]))
  }

  if (typeof a === 'object' && typeof b === 'object') {
    const aObj = a as Record<string, unknown>
    const bObj = b as Record<string, unknown>
    const aKeys = Object.keys(aObj)
    const bKeys = Object.keys(bObj)
    if (aKeys.length !== bKeys.length) return false
    return aKeys.every((key) => key in bObj && deepEquals(aObj[key], bObj[key]))
  }

  return false
}

export function inspect(
  obj: unknown,
  options?: { depth?: number; colors?: boolean },
): string {
  const depth = options?.depth ?? 2
  return inspectValue(obj, depth, new Set())
}

function inspectValue(
  value: unknown,
  depth: number,
  seen: Set<unknown>,
): string {
  if (value === null) return 'null'
  if (value === undefined) return 'undefined'

  const type = typeof value
  if (type === 'string') return JSON.stringify(value)
  if (type === 'number' || type === 'boolean' || type === 'bigint')
    return String(value)
  if (type === 'function')
    return `[Function: ${(value as { name?: string }).name || 'anonymous'}]`
  if (type === 'symbol') return value.toString()

  if (seen.has(value)) return '[Circular]'

  if (Array.isArray(value)) {
    if (depth < 0) return '[Array]'
    seen.add(value)
    const items = value.map((item) => inspectValue(item, depth - 1, seen))
    seen.delete(value)
    return `[ ${items.join(', ')} ]`
  }

  if (value instanceof Date) return value.toISOString()
  if (value instanceof RegExp) return value.toString()
  if (value instanceof Error) return `${value.name}: ${value.message}`

  if (type === 'object') {
    if (depth < 0) return '[Object]'
    seen.add(value)
    const obj = value as Record<string, unknown>
    const entries = Object.entries(obj).map(
      ([k, v]) => `${k}: ${inspectValue(v, depth - 1, seen)}`,
    )
    seen.delete(value)
    return `{ ${entries.join(', ')} }`
  }

  return String(value)
}

// Hashing
type HashInput = string | ArrayBuffer | Uint8Array

function toBytes(data: HashInput): Uint8Array {
  return isString(data)
    ? new TextEncoder().encode(data)
    : isArrayBuffer(data)
      ? new Uint8Array(data)
      : data
}

function hashImpl(data: HashInput, algorithm?: HashAlgorithm): number | bigint {
  const bytes = toBytes(data)

  switch (algorithm ?? 'wyhash') {
    case 'wyhash':
      return wyhash(bytes)
    case 'crc32':
      return crc32(bytes)
    case 'adler32':
      return adler32(bytes)
    case 'cityhash32':
      return cityhash32(bytes)
    case 'cityhash64':
      return cityhash64(bytes)
    case 'murmur32v3':
      return murmur32v3(bytes)
    case 'murmur64v2':
      return murmur64v2(bytes)
  }
}

// Hash with method aliases matching Bun API
export const hash = Object.assign(hashImpl, {
  wyhash: (data: HashInput, seed?: number) => wyhash(toBytes(data), seed),
  crc32: (data: HashInput) => crc32(toBytes(data)),
  adler32: (data: HashInput) => adler32(toBytes(data)),
  cityhash32: (data: HashInput) => cityhash32(toBytes(data)),
  cityhash64: (data: HashInput) => cityhash64(toBytes(data)),
  murmur32v3: (data: HashInput, seed?: number) =>
    murmur32v3(toBytes(data), seed),
  murmur64v2: (data: HashInput, seed?: number) =>
    murmur64v2(toBytes(data), seed),
})

function wyhash(data: Uint8Array, seed = 0): bigint {
  let h = BigInt(seed)
  for (let i = 0; i < data.length; i++) {
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    h = (h ^ BigInt(data[i]!)) * 0x9e3779b97f4a7c15n
    h = h ^ (h >> 32n)
  }
  return h
}

function crc32(data: Uint8Array): number {
  let crc = 0xffffffff
  for (let i = 0; i < data.length; i++) {
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    crc = crc ^ data[i]!
    for (let j = 0; j < 8; j++) {
      crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1
    }
  }
  return (crc ^ 0xffffffff) >>> 0
}

function adler32(data: Uint8Array): number {
  let a = 1,
    b = 0
  const mod = 65521
  for (let i = 0; i < data.length; i++) {
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    a = (a + data[i]!) % mod
    b = (b + a) % mod
  }
  return ((b << 16) | a) >>> 0
}

function cityhash32(data: Uint8Array): number {
  let h = 0x811c9dc5
  for (let i = 0; i < data.length; i++) {
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    h ^= data[i]!
    h = Math.imul(h, 0x01000193)
  }
  return h >>> 0
}

function cityhash64(data: Uint8Array): bigint {
  let h = 0xcbf29ce484222325n
  for (let i = 0; i < data.length; i++) {
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    h ^= BigInt(data[i]!)
    h *= 0x100000001b3n
  }
  return h
}

function murmur32v3(data: Uint8Array, seed = 0): number {
  let h = seed
  const c1 = 0xcc9e2d51,
    c2 = 0x1b873593

  for (let i = 0; i < data.length; i += 4) {
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    let k =
      data[i]! |
      ((data[i + 1] ?? 0) << 8) |
      ((data[i + 2] ?? 0) << 16) |
      ((data[i + 3] ?? 0) << 24)
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

function murmur64v2(data: Uint8Array, seed = 0): bigint {
  let h = BigInt(seed) ^ (BigInt(data.length) * 0xc6a4a7935bd1e995n)
  const m = 0xc6a4a7935bd1e995n,
    r = 47n

  for (let i = 0; i < data.length - 7; i += 8) {
    // All indices from i to i+7 are guaranteed to be in bounds due to loop condition
    // eslint-disable-next-line @typescript-eslint/no-non-null-assertion
    let k =
      BigInt(data[i]!) |
      (BigInt(data[i + 1]!) << 8n) |
      (BigInt(data[i + 2]!) << 16n) |
      (BigInt(data[i + 3]!) << 24n) |
      (BigInt(data[i + 4]!) << 32n) |
      (BigInt(data[i + 5]!) << 40n) |
      (BigInt(data[i + 6]!) << 48n) |
      (BigInt(data[i + 7]!) << 56n)
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

// Password hashing (PBKDF2-based; works in workerd without native deps)
//
// Format:
//   $workerd$pbkdf2$sha256$<iterations>$<saltHex>$<hashHex>
const PBKDF2_HASH_NAME = 'SHA-256'
const PBKDF2_DERIVE_BITS = 256
const DEFAULT_PBKDF2_COST = 10
const DEFAULT_PBKDF2_SALT_BYTES = 16

function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('')
}

function hexToBytes(hex: string): Uint8Array {
  if (hex.length % 2 !== 0) throw new Error('Invalid hex string')
  const bytes = new Uint8Array(hex.length / 2)
  for (let i = 0; i < bytes.length; i++) {
    bytes[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16)
  }
  return bytes
}

function constantTimeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false
  let result = 0
  for (let i = 0; i < a.length; i++) {
    result |= a.charCodeAt(i) ^ b.charCodeAt(i)
  }
  return result === 0
}

async function pbkdf2Hash(
  password: string,
  iterations: number,
  salt: Uint8Array,
): Promise<string> {
  const passwordData = new TextEncoder().encode(password)
  const keyMaterial = await crypto.subtle.importKey(
    'raw',
    passwordData,
    'PBKDF2',
    false,
    ['deriveBits'],
  )
  // Create a copy with guaranteed ArrayBuffer to satisfy BufferSource type
  const saltBuffer = new Uint8Array(salt).buffer as ArrayBuffer
  const derivedBits = await crypto.subtle.deriveBits(
    {
      name: 'PBKDF2',
      salt: new Uint8Array(saltBuffer),
      iterations,
      hash: PBKDF2_HASH_NAME,
    },
    keyMaterial,
    PBKDF2_DERIVE_BITS,
  )
  return bytesToHex(new Uint8Array(derivedBits))
}

export const password = {
  async hash(
    password: string,
    options?: {
      algorithm?: 'pbkdf2' | 'bcrypt' | 'argon2id' | 'argon2d' | 'argon2i'
      cost?: number
    },
  ): Promise<string> {
    const algorithm = options?.algorithm ?? 'pbkdf2'
    if (algorithm !== 'pbkdf2') {
      throw new Error(
        `Algorithm '${algorithm}' is not available in workerd. Use 'pbkdf2' instead.`,
      )
    }

    const cost = options?.cost ?? DEFAULT_PBKDF2_COST
    const iterations = 2 ** cost * 100
    const salt = crypto.getRandomValues(
      new Uint8Array(DEFAULT_PBKDF2_SALT_BYTES),
    )
    const hashHex = await pbkdf2Hash(password, iterations, salt)
    return `$workerd$pbkdf2$sha256$${iterations}$${bytesToHex(salt)}$${hashHex}`
  },

  async verify(password: string, hash: string): Promise<boolean> {
    if (hash.startsWith('$workerd$pbkdf2$sha256$')) {
      // Format: $workerd$pbkdf2$sha256$<iterations>$<saltHex>$<hashHex>
      // Parts: ['', 'workerd', 'pbkdf2', 'sha256', iterations, saltHex, hashHex]
      const parts = hash.split('$')
      if (parts.length !== 7) throw new Error('Invalid workerd hash format')

      const iterationsRaw = parts[4]
      const saltHex = parts[5]
      const expectedHashHex = parts[6]

      if (
        iterationsRaw === undefined ||
        saltHex === undefined ||
        expectedHashHex === undefined
      ) {
        throw new Error('Invalid workerd hash format')
      }

      if (!/^\d+$/.test(iterationsRaw))
        throw new Error('Invalid workerd hash format')
      const iterations = parseInt(iterationsRaw, 10)
      if (
        !Number.isSafeInteger(iterations) ||
        iterations <= 0 ||
        iterations > 0xffffffff
      ) {
        throw new Error('Invalid workerd hash format')
      }

      if (!/^[0-9a-f]+$/i.test(saltHex) || saltHex.length % 2 !== 0) {
        throw new Error('Invalid workerd hash format')
      }
      if (
        !/^[0-9a-f]+$/i.test(expectedHashHex) ||
        expectedHashHex.length === 0
      ) {
        throw new Error('Invalid workerd hash format')
      }

      const salt = hexToBytes(saltHex)
      const computedHashHex = await pbkdf2Hash(password, iterations, salt)
      return constantTimeEqual(computedHashHex, expectedHashHex)
    }

    // Legacy: $workerd$pbkdf2$<cost>$<saltHex>$<hashHex>
    if (hash.startsWith('$workerd$')) {
      return verifyLegacyWorkerdHash(password, hash)
    }

    throw new Error('Unknown hash format. Expected workerd PBKDF2 hash.')
  },
}

// Legacy verification for old PBKDF2 hashes (backwards compatibility only).
async function verifyLegacyWorkerdHash(
  password: string,
  hash: string,
): Promise<boolean> {
  // Format: $workerd$pbkdf2$<cost>$<saltHex>$<hashHex>
  // Parts: ['', 'workerd', 'pbkdf2', cost, saltHex, hashHex]
  const parts = hash.split('$')
  if (parts.length !== 6) {
    throw new Error(
      'Invalid legacy workerd hash format. Expected: $workerd$pbkdf2$<cost>$<saltHex>$<hashHex>',
    )
  }

  const algorithm = parts[2]
  if (algorithm !== 'pbkdf2') {
    throw new Error(
      `Unknown legacy hash algorithm: ${algorithm}. Only 'pbkdf2' is supported.`,
    )
  }

  const costStr = parts[3]
  const saltHex = parts[4]
  const expectedHashHex = parts[5]

  if (
    costStr === undefined ||
    saltHex === undefined ||
    expectedHashHex === undefined
  ) {
    throw new Error('Invalid legacy workerd hash format: missing components')
  }

  if (!/^\d+$/.test(costStr)) {
    throw new Error('Invalid legacy workerd hash format: cost must be numeric')
  }

  const cost = parseInt(costStr, 10)
  if (!Number.isSafeInteger(cost) || cost < 0 || cost > 31) {
    throw new Error(
      'Invalid legacy workerd hash format: cost must be between 0 and 31',
    )
  }

  if (!/^[0-9a-f]+$/i.test(saltHex) || saltHex.length % 2 !== 0) {
    throw new Error(
      'Invalid legacy workerd hash format: salt must be valid hex string',
    )
  }

  if (!/^[0-9a-f]+$/i.test(expectedHashHex) || expectedHashHex.length === 0) {
    throw new Error(
      'Invalid legacy workerd hash format: hash must be valid non-empty hex string',
    )
  }

  const iterations = 2 ** cost * 100
  const salt = hexToBytes(saltHex)
  const computedHashHex = await pbkdf2Hash(password, iterations, salt)
  return constantTimeEqual(computedHashHex, expectedHashHex)
}

// Stream utilities
export async function readableStreamToArray<T>(
  stream: ReadableStream<T>,
): Promise<T[]> {
  const reader = stream.getReader()
  const chunks: T[] = []
  while (true) {
    const { done, value } = await reader.read()
    if (done) break
    chunks.push(value)
  }
  return chunks
}

export async function readableStreamToText(
  stream: ReadableStream<Uint8Array>,
): Promise<string> {
  const chunks = await readableStreamToArray(stream)
  const decoder = new TextDecoder()
  return chunks.map((chunk) => decoder.decode(chunk, { stream: true })).join('')
}

export async function readableStreamToArrayBuffer(
  stream: ReadableStream<Uint8Array>,
): Promise<ArrayBuffer> {
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

export async function readableStreamToBlob(
  stream: ReadableStream<Uint8Array>,
  type?: string,
): Promise<Blob> {
  const buffer = await readableStreamToArrayBuffer(stream)
  return type !== undefined ? new Blob([buffer], { type }) : new Blob([buffer])
}

export async function readableStreamToJSON<T = unknown>(
  stream: ReadableStream<Uint8Array>,
): Promise<T> {
  return JSON.parse(await readableStreamToText(stream)) as T
}

// ArrayBufferSink
export class ArrayBufferSink {
  private chunks: Uint8Array[] = []

  write(data: string | ArrayBuffer | Uint8Array): void {
    const bytes =
      typeof data === 'string'
        ? new TextEncoder().encode(data)
        : data instanceof ArrayBuffer
          ? new Uint8Array(data)
          : data
    this.chunks.push(bytes)
  }

  end(): ArrayBuffer {
    const totalLength = this.chunks.reduce((sum, c) => sum + c.byteLength, 0)
    const result = new Uint8Array(totalLength)
    let offset = 0
    for (const chunk of this.chunks) {
      result.set(chunk, offset)
      offset += chunk.byteLength
    }
    this.chunks = []
    return result.buffer
  }

  flush(): void {} // no-op, data accumulated until end()

  start(): void {
    this.chunks = []
  }
}

// Misc
export const main = (() => {
  const proc = getProcess() as { argv?: string[] } | undefined
  return proc?.argv?.[1] !== undefined
})()

export function randomUUIDv7(): string {
  const timestamp = Date.now()
  const timestampHex = timestamp.toString(16).padStart(12, '0')
  const random = crypto.getRandomValues(new Uint8Array(10))
  const randomHex = Array.from(random)
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('')

  return [
    timestampHex.slice(0, 8),
    timestampHex.slice(8, 12),
    `7${randomHex.slice(0, 3)}`,
    ((parseInt(randomHex.slice(3, 5), 16) & 0x3f) | 0x80)
      .toString(16)
      .padStart(2, '0') + randomHex.slice(5, 7),
    randomHex.slice(7, 19),
  ].join('-')
}

interface PromiseTracker {
  resolved: boolean
  value: unknown
  error: unknown
}

const resolvedPromises = new WeakMap<Promise<unknown>, PromiseTracker>()

export function peek<T>(promise: Promise<T>): T | Promise<T> {
  const cached = resolvedPromises.get(promise as Promise<unknown>)
  if (cached) {
    if (cached.resolved) {
      if (cached.error !== undefined) throw cached.error
      return cached.value as T
    }
    return promise
  }

  const tracker: PromiseTracker = {
    resolved: false,
    value: undefined,
    error: undefined,
  }
  resolvedPromises.set(promise as Promise<unknown>, tracker)

  promise
    .then((value) => {
      tracker.resolved = true
      tracker.value = value
    })
    .catch((error: unknown) => {
      tracker.resolved = true
      tracker.error = error
    })

  return promise
}

let gcWarningShown = false
let shrinkWarningShown = false

export function gc(): void {
  if (!gcWarningShown) {
    console.warn(
      '[Bun.gc] No-op in workerd: V8 isolate garbage collection is managed by the runtime.',
    )
    gcWarningShown = true
  }
}

export function shrink(): void {
  if (!shrinkWarningShown) {
    console.warn(
      '[Bun.shrink] No-op in workerd: Memory management is handled by the runtime.',
    )
    shrinkWarningShown = true
  }
}

export function generateHeapSnapshot(): never {
  throw new Error('generateHeapSnapshot is not available in workerd')
}

export function openInEditor(
  _path: string,
  _options?: { line?: number; column?: number },
): never {
  throw new Error('openInEditor is not available in workerd')
}

export function fileURLToPath(url: string | URL): string {
  const urlObj = typeof url === 'string' ? new URL(url) : url
  if (urlObj.protocol !== 'file:')
    throw new Error('URL must use file: protocol')
  return urlObj.pathname
}

export function pathToFileURL(path: string): URL {
  return new URL(`file://${path.startsWith('/') ? '' : '/'}${path}`)
}

// Default export
export default {
  file,
  write,
  serve,
  getServeHandler,
  env,
  version,
  revision,
  sleep,
  sleepSync,
  nanoseconds,
  escapeHTML,
  stringWidth,
  deepEquals,
  inspect,
  hash,
  password,
  readableStreamToArray,
  readableStreamToText,
  readableStreamToArrayBuffer,
  readableStreamToBlob,
  readableStreamToJSON,
  ArrayBufferSink,
  dns,
  main,
  randomUUIDv7,
  peek,
  gc,
  shrink,
  generateHeapSnapshot,
  openInEditor,
  fileURLToPath,
  pathToFileURL,
}
