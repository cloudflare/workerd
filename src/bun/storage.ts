/**
 * DWS Storage Client for Bun.file/write
 *
 * Provides REAL file storage using DWS (IPFS-based decentralized storage).
 *
 * Path formats:
 * - `dws://cid` - Download from DWS by CID
 * - `ipfs://cid` - Download from IPFS gateway
 * - `dws://upload/filename` - Upload to DWS
 * - `:memory:path` - In-memory storage (default fallback)
 */

import { ERR_FS_FILE_NOT_FOUND } from 'bun-internal:errors'

// Storage configuration
interface DWSConfig {
  endpoint: string // DWS API endpoint (e.g., http://localhost:4030)
  ipfsGateway: string // IPFS gateway (e.g., https://ipfs.io)
  authAddress?: string // Optional auth address header
}

let dwsConfig: DWSConfig = {
  endpoint: process.env.DWS_ENDPOINT ?? 'http://localhost:4030',
  ipfsGateway: process.env.IPFS_GATEWAY ?? 'https://ipfs.io',
}

/**
 * Configure DWS storage
 */
export function configure(config: Partial<DWSConfig>): void {
  dwsConfig = { ...dwsConfig, ...config }
}

/**
 * Get current configuration
 */
export function getConfig(): DWSConfig {
  return { ...dwsConfig }
}

// In-memory storage (fallback for :memory: paths and testing)
const memoryStorage = new Map<string, Uint8Array>()
const memoryMetadata = new Map<string, { type: string; lastModified: number; cid?: string }>()

// CID cache for uploaded files (path -> cid mapping)
const cidCache = new Map<string, string>()

/**
 * Parse a storage path to determine backend and resource
 */
function parseStoragePath(path: string): {
  backend: 'dws' | 'ipfs' | 'memory'
  resource: string
  isUpload: boolean
} {
  if (path.startsWith('dws://')) {
    const rest = path.slice(6)
    if (rest.startsWith('upload/')) {
      return { backend: 'dws', resource: rest.slice(7), isUpload: true }
    }
    return { backend: 'dws', resource: rest, isUpload: false }
  }

  if (path.startsWith('ipfs://')) {
    return { backend: 'ipfs', resource: path.slice(7), isUpload: false }
  }

  // Default to memory storage
  return { backend: 'memory', resource: path, isUpload: false }
}

/**
 * Upload content to DWS Storage
 */
export async function upload(
  content: Uint8Array | string,
  options?: { filename?: string; permanent?: boolean },
): Promise<{ cid: string; url: string }> {
  const data = typeof content === 'string' ? new TextEncoder().encode(content) : content
  const filename = options?.filename ?? 'file'

  const formData = new FormData()
  formData.append('file', new Blob([data]), filename)
  if (options?.permanent) {
    formData.append('permanent', 'true')
  }

  const headers: Record<string, string> = {}
  if (dwsConfig.authAddress) {
    headers['x-sender-address'] = dwsConfig.authAddress
  }

  const response = await fetch(`${dwsConfig.endpoint}/storage/upload`, {
    method: 'POST',
    headers,
    body: formData,
  })

  if (!response.ok) {
    const text = await response.text()
    throw new Error(`DWS upload failed: ${response.status} ${text}`)
  }

  const result = (await response.json()) as { cid: string; url?: string }
  return {
    cid: result.cid,
    url: result.url ?? `${dwsConfig.ipfsGateway}/ipfs/${result.cid}`,
  }
}

/**
 * Download content from DWS/IPFS
 */
export async function download(cid: string): Promise<Uint8Array> {
  // Try DWS first, then IPFS gateway
  const urls = [
    `${dwsConfig.endpoint}/storage/download/${cid}`,
    `${dwsConfig.ipfsGateway}/ipfs/${cid}`,
  ]

  for (const url of urls) {
    try {
      const response = await fetch(url)
      if (response.ok) {
        return new Uint8Array(await response.arrayBuffer())
      }
    } catch {
      // Try next URL
    }
  }

  throw new ERR_FS_FILE_NOT_FOUND(`dws://${cid}`)
}

/**
 * Check if content exists
 */
export async function exists(cid: string): Promise<boolean> {
  try {
    const response = await fetch(`${dwsConfig.endpoint}/storage/download/${cid}`, {
      method: 'HEAD',
    })
    return response.ok
  } catch {
    return false
  }
}

// =============================================================================
// BunFile Implementation with DWS Support
// =============================================================================

export interface DWSBunFile {
  readonly size: number
  readonly type: string
  readonly name: string
  readonly lastModified: number
  readonly cid?: string
  text(): Promise<string>
  json<T = unknown>(): Promise<T>
  arrayBuffer(): Promise<ArrayBuffer>
  bytes(): Promise<Uint8Array>
  stream(): ReadableStream<Uint8Array>
  slice(start?: number, end?: number, type?: string): DWSBunFile
  exists(): Promise<boolean>
  writer(): DWSFileSink
}

export interface DWSFileSink {
  write(data: string | ArrayBuffer | Uint8Array): number
  flush(): Promise<void>
  end(): Promise<{ cid: string }>
}

class DWSBunFileImpl implements DWSBunFile {
  private readonly path: string
  private readonly _type: string
  private _cachedData?: Uint8Array
  private _cid?: string

  constructor(path: string | URL, options?: { type?: string }) {
    this.path = typeof path === 'string' ? path : path.pathname
    this._type = options?.type ?? 'application/octet-stream'
    this._cid = cidCache.get(this.path)
  }

  get cid(): string | undefined {
    return this._cid ?? cidCache.get(this.path)
  }

  get size(): number {
    const parsed = parseStoragePath(this.path)
    if (parsed.backend === 'memory') {
      return memoryStorage.get(parsed.resource)?.byteLength ?? 0
    }
    // For remote files, we don't know size until downloaded
    return this._cachedData?.byteLength ?? 0
  }

  get type(): string {
    const parsed = parseStoragePath(this.path)
    if (parsed.backend === 'memory') {
      return memoryMetadata.get(parsed.resource)?.type ?? this._type
    }
    return this._type
  }

  get name(): string {
    return this.path
  }

  get lastModified(): number {
    const parsed = parseStoragePath(this.path)
    if (parsed.backend === 'memory') {
      return memoryMetadata.get(parsed.resource)?.lastModified ?? Date.now()
    }
    return Date.now()
  }

  private async getData(): Promise<Uint8Array> {
    if (this._cachedData) return this._cachedData

    const parsed = parseStoragePath(this.path)

    if (parsed.backend === 'memory') {
      const data = memoryStorage.get(parsed.resource)
      if (!data) throw new ERR_FS_FILE_NOT_FOUND(this.path)
      return data
    }

    // Download from DWS/IPFS
    this._cachedData = await download(parsed.resource)
    return this._cachedData
  }

  async text(): Promise<string> {
    const data = await this.getData()
    return new TextDecoder().decode(data)
  }

  async json<T = unknown>(): Promise<T> {
    return JSON.parse(await this.text()) as T
  }

  async arrayBuffer(): Promise<ArrayBuffer> {
    const data = await this.getData()
    const ab = data.buffer
    if (ab instanceof ArrayBuffer) {
      return ab.slice(data.byteOffset, data.byteOffset + data.byteLength)
    }
    const copy = new ArrayBuffer(data.byteLength)
    new Uint8Array(copy).set(data)
    return copy
  }

  async bytes(): Promise<Uint8Array> {
    return this.getData()
  }

  stream(): ReadableStream<Uint8Array> {
    const path = this.path
    const parsed = parseStoragePath(path)

    return new ReadableStream({
      async start(controller) {
        try {
          let data: Uint8Array
          if (parsed.backend === 'memory') {
            const stored = memoryStorage.get(parsed.resource)
            if (!stored) throw new ERR_FS_FILE_NOT_FOUND(path)
            data = stored
          } else {
            data = await download(parsed.resource)
          }
          controller.enqueue(data)
          controller.close()
        } catch (err) {
          controller.error(err)
        }
      },
    })
  }

  slice(start?: number, end?: number, type?: string): DWSBunFile {
    // For slicing, we need to download the data first
    // Return a new file that will slice when read
    const slicePath = `${this.path}#slice(${start},${end})`
    return new SlicedDWSBunFile(this, start, end, type ?? this._type, slicePath)
  }

  async exists(): Promise<boolean> {
    const parsed = parseStoragePath(this.path)

    if (parsed.backend === 'memory') {
      return memoryStorage.has(parsed.resource)
    }

    return exists(parsed.resource)
  }

  writer(): DWSFileSink {
    const path = this.path
    const chunks: Uint8Array[] = []
    const parsed = parseStoragePath(path)

    return {
      write(data: string | ArrayBuffer | Uint8Array): number {
        const bytes =
          typeof data === 'string'
            ? new TextEncoder().encode(data)
            : data instanceof ArrayBuffer
              ? new Uint8Array(data)
              : data
        chunks.push(bytes)
        return bytes.byteLength
      },

      async flush(): Promise<void> {
        const totalLength = chunks.reduce((sum, c) => sum + c.byteLength, 0)
        const result = new Uint8Array(totalLength)
        let offset = 0
        for (const chunk of chunks) {
          result.set(chunk, offset)
          offset += chunk.byteLength
        }

        if (parsed.backend === 'memory' || parsed.backend === 'dws' && !parsed.isUpload) {
          // Store in memory
          memoryStorage.set(parsed.resource, result)
          memoryMetadata.set(parsed.resource, {
            type: 'application/octet-stream',
            lastModified: Date.now(),
          })
        } else {
          // Upload to DWS
          const uploadResult = await upload(result, { filename: parsed.resource })
          cidCache.set(path, uploadResult.cid)
          memoryMetadata.set(path, {
            type: 'application/octet-stream',
            lastModified: Date.now(),
            cid: uploadResult.cid,
          })
        }
      },

      async end(): Promise<{ cid: string }> {
        await this.flush()
        chunks.length = 0
        const cid = cidCache.get(path) ?? ''
        return { cid }
      },
    }
  }
}

// Sliced file that reads from parent
class SlicedDWSBunFile implements DWSBunFile {
  private parent: DWSBunFile
  private start?: number
  private end?: number
  private _type: string
  private _path: string

  constructor(
    parent: DWSBunFile,
    start: number | undefined,
    end: number | undefined,
    type: string,
    path: string,
  ) {
    this.parent = parent
    this.start = start
    this.end = end
    this._type = type
    this._path = path
  }

  get size(): number {
    const parentSize = this.parent.size
    const start = this.start ?? 0
    const end = this.end ?? parentSize
    return Math.max(0, Math.min(end, parentSize) - start)
  }

  get type(): string {
    return this._type
  }

  get name(): string {
    return this._path
  }

  get lastModified(): number {
    return this.parent.lastModified
  }

  get cid(): string | undefined {
    return undefined // Sliced files don't have their own CID
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
    return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength)
  }

  async bytes(): Promise<Uint8Array> {
    const parentData = await this.parent.bytes()
    return parentData.slice(this.start, this.end)
  }

  stream(): ReadableStream<Uint8Array> {
    const self = this
    return new ReadableStream({
      async start(controller) {
        const data = await self.bytes()
        controller.enqueue(data)
        controller.close()
      },
    })
  }

  slice(start?: number, end?: number, type?: string): DWSBunFile {
    // Nested slicing
    const actualStart = (this.start ?? 0) + (start ?? 0)
    const actualEnd = this.end !== undefined ? Math.min((this.start ?? 0) + (end ?? this.end), this.end) : end
    return new SlicedDWSBunFile(this.parent, actualStart, actualEnd, type ?? this._type, `${this._path}#slice(${start},${end})`)
  }

  async exists(): Promise<boolean> {
    return this.parent.exists()
  }

  writer(): DWSFileSink {
    throw new Error('Cannot write to a sliced file')
  }
}

// =============================================================================
// Exports
// =============================================================================

/**
 * Create a file reference (works with dws://, ipfs://, or memory paths)
 */
export function file(path: string | URL, options?: { type?: string }): DWSBunFile {
  return new DWSBunFileImpl(path, options)
}

/**
 * Write data to storage
 */
export async function write(
  destination: string | URL | DWSBunFile,
  data: string | ArrayBuffer | Uint8Array | Blob | Response | DWSBunFile,
): Promise<number> {
  const path =
    typeof destination === 'string'
      ? destination
      : destination instanceof URL
        ? destination.pathname
        : destination.name

  let bytes: Uint8Array
  if (typeof data === 'string') {
    bytes = new TextEncoder().encode(data)
  } else if (data instanceof ArrayBuffer) {
    bytes = new Uint8Array(data)
  } else if (data instanceof Uint8Array) {
    bytes = data
  } else if (data instanceof Blob) {
    bytes = new Uint8Array(await data.arrayBuffer())
  } else if (data instanceof Response) {
    bytes = new Uint8Array(await data.arrayBuffer())
  } else {
    bytes = await data.bytes()
  }

  const parsed = parseStoragePath(path)

  if (parsed.backend === 'dws' && parsed.isUpload) {
    // Upload to DWS
    const result = await upload(bytes, { filename: parsed.resource })
    cidCache.set(path, result.cid)
    memoryMetadata.set(path, {
      type: 'application/octet-stream',
      lastModified: Date.now(),
      cid: result.cid,
    })
  } else {
    // Store in memory
    memoryStorage.set(parsed.resource, bytes)
    memoryMetadata.set(parsed.resource, {
      type: 'application/octet-stream',
      lastModified: Date.now(),
    })
  }

  return bytes.byteLength
}

/**
 * Get CID for a previously uploaded file
 */
export function getCid(path: string): string | undefined {
  return cidCache.get(path) ?? memoryMetadata.get(path)?.cid
}

/**
 * Clear memory storage (for testing)
 */
export function clearMemory(): void {
  memoryStorage.clear()
  memoryMetadata.clear()
  cidCache.clear()
}

export default {
  configure,
  getConfig,
  upload,
  download,
  exists,
  file,
  write,
  getCid,
  clearMemory,
}
