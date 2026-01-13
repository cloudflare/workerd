// Copyright (c) 2024 Jeju Network
// Bun-compatible worker sample for workerd with embedded Bun bundle
// Licensed under the Apache 2.0 license

// This worker demonstrates Bun APIs running in workerd using the standalone bundle
// The Bun APIs are embedded directly in the worker until native bun: support is built

// ============================================================================
// Embedded Bun Bundle - Start
// ============================================================================

const ERR_WORKERD_UNAVAILABLE = (api) => new Error(`${api} is not available in workerd`)
const ERR_FS_FILE_NOT_FOUND = (path) => new Error(`ENOENT: no such file or directory, open '${path}'`)

const isString = (v) => typeof v === 'string'
const isUint8Array = (v) => v instanceof Uint8Array
const isArrayBuffer = (v) => v instanceof ArrayBuffer

const virtualFS = new Map()

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
  
  get type() { return this.#type }
  get name() { return this.#path }
  get lastModified() { return Date.now() }
  
  async text() {
    const data = virtualFS.get(this.#path)
    if (!data) throw ERR_FS_FILE_NOT_FOUND(this.#path)
    return new TextDecoder().decode(data)
  }
  
  async json() {
    return JSON.parse(await this.text())
  }
  
  async arrayBuffer() {
    const data = virtualFS.get(this.#path)
    if (!data) throw ERR_FS_FILE_NOT_FOUND(this.#path)
    return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength)
  }
  
  async bytes() {
    const data = virtualFS.get(this.#path)
    if (!data) throw ERR_FS_FILE_NOT_FOUND(this.#path)
    return new Uint8Array(data)
  }
  
  stream() {
    const data = virtualFS.get(this.#path)
    if (!data) {
      return new ReadableStream({
        start(controller) { controller.error(ERR_FS_FILE_NOT_FOUND(this.#path)) }
      })
    }
    return new ReadableStream({
      start(controller) {
        controller.enqueue(data)
        controller.close()
      }
    })
  }
  
  slice(start = 0, end = this.size, type = this.#type) {
    return new BunFileImpl(this.#path, { type })
  }
  
  async exists() {
    return virtualFS.has(this.#path)
  }
  
  writer() {
    const path = this.#path
    const chunks = []
    return {
      write(data) {
        const bytes = isString(data) ? new TextEncoder().encode(data) : new Uint8Array(data)
        chunks.push(bytes)
        return bytes.length
      },
      flush() {},
      end() {
        const total = chunks.reduce((acc, c) => acc + c.length, 0)
        const result = new Uint8Array(total)
        let offset = 0
        for (const chunk of chunks) {
          result.set(chunk, offset)
          offset += chunk.length
        }
        virtualFS.set(path, result)
      }
    }
  }
}

function fastHash(data) {
  const bytes = isString(data) ? new TextEncoder().encode(data) : new Uint8Array(data)
  let hash = 0
  for (let i = 0; i < bytes.length; i++) {
    hash = ((hash << 5) - hash) + bytes[i]
    hash = hash >>> 0
  }
  return hash
}

function deepEquals(a, b, strict = false) {
  if (a === b) return true
  if (a === null || b === null) return false
  if (typeof a !== typeof b) return false
  
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false
    return a.every((v, i) => deepEquals(v, b[i], strict))
  }
  
  if (typeof a === 'object') {
    const keysA = Object.keys(a)
    const keysB = Object.keys(b)
    if (keysA.length !== keysB.length) return false
    return keysA.every(key => deepEquals(a[key], b[key], strict))
  }
  
  return false
}

function escapeHTML(str) {
  return str
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
}

function stringWidth(str) {
  let width = 0
  for (const char of str) {
    const code = char.codePointAt(0)
    if (code <= 0x1f || (code >= 0x7f && code <= 0x9f)) continue
    if (code >= 0x1100 && (code <= 0x115f || code === 0x2329 || code === 0x232a ||
        (code >= 0x2e80 && code <= 0xa4cf && code !== 0x303f) ||
        (code >= 0xac00 && code <= 0xd7a3) ||
        (code >= 0xf900 && code <= 0xfaff) ||
        (code >= 0xfe10 && code <= 0xfe1f) ||
        (code >= 0xfe30 && code <= 0xfe6f) ||
        (code >= 0xff00 && code <= 0xff60) ||
        (code >= 0xffe0 && code <= 0xffe6) ||
        (code >= 0x20000 && code <= 0x2fffd) ||
        (code >= 0x30000 && code <= 0x3fffd))) {
      width += 2
    } else {
      width += 1
    }
  }
  return width
}

function inspect(value, options = {}) {
  const { depth = 2, colors = false } = options
  return JSON.stringify(value, (key, val) => {
    if (typeof val === 'function') return '[Function]'
    if (val instanceof Date) return val.toISOString()
    if (val instanceof RegExp) return val.toString()
    return val
  }, 2)
}

let perfStartTime = typeof performance !== 'undefined' ? performance.now() : Date.now()
function nanoseconds() {
  const now = typeof performance !== 'undefined' ? performance.now() : Date.now()
  return BigInt(Math.floor((now - perfStartTime) * 1_000_000))
}

async function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms))
}

function sleepSync(ms) {
  const end = Date.now() + ms
  while (Date.now() < end) {}
}

async function write(dest, data) {
  const destPath = typeof dest === 'string' ? dest : dest.name
  let bytes
  
  if (isString(data)) {
    bytes = new TextEncoder().encode(data)
  } else if (isUint8Array(data)) {
    bytes = data
  } else if (isArrayBuffer(data)) {
    bytes = new Uint8Array(data)
  } else if (data instanceof Blob) {
    bytes = new Uint8Array(await data.arrayBuffer())
  } else if (data instanceof Response) {
    bytes = new Uint8Array(await data.arrayBuffer())
  } else if (data instanceof BunFileImpl) {
    bytes = await data.bytes()
  } else {
    throw new TypeError('Invalid data type for Bun.write')
  }
  
  virtualFS.set(destPath, bytes)
  return bytes.length
}

const Bun = {
  version: '1.0.0-workerd',
  revision: 'workerd',
  main: '',
  env: {},
  file: (path, options) => new BunFileImpl(path, options),
  write,
  hash: fastHash,
  sleep,
  sleepSync,
  escapeHTML,
  stringWidth,
  deepEquals,
  inspect,
  nanoseconds,
  serve: () => { throw ERR_WORKERD_UNAVAILABLE('Bun.serve') },
}

// ============================================================================
// Embedded Bun Bundle - End
// ============================================================================

const startTime = Date.now()

export default {
  async fetch(request) {
    const url = new URL(request.url)
    
    switch (url.pathname) {
      case '/':
        return new Response(JSON.stringify({
          message: 'Hello from Bun worker!',
          runtime: 'workerd',
          bunVersion: Bun.version,
          uptime: Date.now() - startTime,
          timestamp: new Date().toISOString()
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/bun-apis':
        // Test various Bun APIs
        const testData = 'Hello, Bun!'
        const hashResult = Bun.hash(testData)
        const escapedHtml = Bun.escapeHTML('<script>alert("xss")</script>')
        const width = Bun.stringWidth('Hello 👋')
        const equal = Bun.deepEquals({ a: 1 }, { a: 1 })
        const inspected = Bun.inspect({ nested: { value: 123 } })
        const ns = Bun.nanoseconds()
        
        return new Response(JSON.stringify({
          hash: hashResult.toString(16),
          escapedHtml,
          stringWidth: width,
          deepEquals: equal,
          inspected,
          nanoseconds: ns.toString()
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/file-ops':
        // Test file operations
        await Bun.write('/test.txt', 'Hello from Bun file API!')
        const file = Bun.file('/test.txt')
        const content = await file.text()
        const exists = await file.exists()
        
        return new Response(JSON.stringify({
          written: true,
          content,
          exists,
          size: file.size
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/deep-equals':
        const obj1 = { a: 1, b: { c: [1, 2, 3] } }
        const obj2 = { a: 1, b: { c: [1, 2, 3] } }
        const obj3 = { a: 1, b: { c: [1, 2, 4] } }
        
        return new Response(JSON.stringify({
          obj1_equals_obj2: Bun.deepEquals(obj1, obj2),
          obj1_equals_obj3: Bun.deepEquals(obj1, obj3)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/escape-html':
        const html = url.searchParams.get('html') || '<script>alert("xss")</script>'
        
        return new Response(JSON.stringify({
          input: html,
          escaped: Bun.escapeHTML(html)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/inspect':
        const complexObj = {
          name: 'test',
          nested: { deep: { value: [1, 2, 3] } },
          date: new Date()
        }
        
        return new Response(JSON.stringify({
          inspected: Bun.inspect(complexObj)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/hash':
        const data = url.searchParams.get('data') || 'hello'
        const hash = Bun.hash(data)
        
        return new Response(JSON.stringify({
          input: data,
          bunHash: hash.toString(16)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/nanoseconds':
        const nsValue = Bun.nanoseconds()
        
        return new Response(JSON.stringify({
          nanoseconds: nsValue.toString(),
          milliseconds: Math.floor(Number(nsValue) / 1_000_000)
        }), {
          headers: { 'content-type': 'application/json' }
        })
      
      case '/health':
        return new Response('OK', {
          status: 200,
          headers: { 'content-type': 'text/plain' }
        })
      
      default:
        return new Response(JSON.stringify({
          error: 'Not Found',
          path: url.pathname,
          availableRoutes: [
            '/',
            '/bun-apis',
            '/file-ops',
            '/deep-equals',
            '/escape-html',
            '/inspect',
            '/hash',
            '/nanoseconds',
            '/health'
          ]
        }), {
          status: 404,
          headers: { 'content-type': 'application/json' }
        })
    }
  }
}
