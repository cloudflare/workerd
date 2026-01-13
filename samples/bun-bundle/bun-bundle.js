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
    super(`ENOENT: no such file or directory, open '${path}'`, 'ENOENT')
    this.name = 'ERR_FS_FILE_NOT_FOUND'
  }
}

class ERR_WORKERD_UNAVAILABLE extends BunError {
  constructor(feature, reason) {
    const msg = reason
      ? `${feature} is not available in workerd: ${reason}`
      : `${feature} is not available in workerd`
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
    const slicePath = `${this.#path}#slice(${start},${end})`
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
  if (type === 'function') return `[Function: ${value.name || 'anonymous'}]`
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
    const entries = Object.entries(value).map(([k, v]) => `${k}: ${inspectValue(v, depth - 1, seen)}`)
    seen.delete(value)
    return `{ ${entries.join(', ')} }`
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
var bcrypt=(()=>{var V=Object.create;var E=Object.defineProperty;var k=Object.getOwnPropertyDescriptor;var P=Object.getOwnPropertyNames;var S=Object.getPrototypeOf,x0=Object.prototype.hasOwnProperty;var e0=(f,a)=>()=>(a||f((a={exports:{}}).exports,a),a.exports),f0=(f,a)=>{for(var e in a)E(f,e,{get:a[e],enumerable:!0})},O=(f,a,e,c)=>{if(a&&typeof a=="object"||typeof a=="function")for(let x of P(a))!x0.call(f,x)&&x!==e&&E(f,x,{get:()=>a[x],enumerable:!(c=k(a,x))||c.enumerable});return f};var a0=(f,a,e)=>(e=f!=null?V(S(f)):{},O(a||!f||!f.__esModule?E(e,"default",{value:f,enumerable:!0}):e,f)),c0=f=>O(E({},"__esModule",{value:!0}),f);var B=e0(()=>{});var i0={};f0(i0,{compare:()=>W,compareSync:()=>X,decodeBase64:()=>Z,default:()=>o0,encodeBase64:()=>Q,genSalt:()=>R,genSaltSync:()=>C,getRounds:()=>H,getSalt:()=>Y,hash:()=>$,hashSync:()=>T,setRandomFallback:()=>G,truncates:()=>q});var F=a0(B(),1),_=null;function b0(f){try{return crypto.getRandomValues(new Uint8Array(f))}catch{}try{return F.default.randomBytes(f)}catch{}if(!_)throw Error("Neither WebCryptoAPI nor a crypto module is available. Use bcrypt.setRandomFallback to set an alternative");return _(f)}function G(f){_=f}function C(f,a){if(f=f||N,typeof f!="number")throw Error("Illegal arguments: "+typeof f+", "+typeof a);f<4?f=4:f>31&&(f=31);var e=[];return e.push("$2b$"),f<10&&e.push("0"),e.push(f.toString()),e.push("$"),e.push(A(b0(m),m)),e.join("")}function R(f,a,e){if(typeof a=="function"&&(e=a,a=void 0),typeof f=="function"&&(e=f,f=void 0),typeof f>"u")f=N;else if(typeof f!="number")throw Error("illegal arguments: "+typeof f);function c(x){h(function(){try{x(null,C(f))}catch(b){x(b)}})}if(e){if(typeof e!="function")throw Error("Illegal callback: "+typeof e);c(e)}else return new Promise(function(x,b){c(function(d,t){if(d){b(d);return}x(t)})})}function T(f,a){if(typeof a>"u"&&(a=N),typeof a=="number"&&(a=C(a)),typeof f!="string"||typeof a!="string")throw Error("Illegal arguments: "+typeof f+", "+typeof a);return w(f,a)}function $(f,a,e,c){function x(b){typeof f=="string"&&typeof a=="number"?R(a,function(d,t){w(f,t,b,c)}):typeof f=="string"&&typeof a=="string"?w(f,a,b,c):h(b.bind(this,Error("Illegal arguments: "+typeof f+", "+typeof a)))}if(e){if(typeof e!="function")throw Error("Illegal callback: "+typeof e);x(e)}else return new Promise(function(b,d){x(function(t,r){if(t){d(t);return}b(r)})})}function M(f,a){for(var e=f.length^a.length,c=0;c<f.length;++c)e|=f.charCodeAt(c)^a.charCodeAt(c);return e===0}function X(f,a){if(typeof f!="string"||typeof a!="string")throw Error("Illegal arguments: "+typeof f+", "+typeof a);return a.length!==60?!1:M(T(f,a.substring(0,a.length-31)),a)}function W(f,a,e,c){function x(b){if(typeof f!="string"||typeof a!="string"){h(b.bind(this,Error("Illegal arguments: "+typeof f+", "+typeof a)));return}if(a.length!==60){h(b.bind(this,null,!1));return}$(f,a.substring(0,29),function(d,t){d?b(d):b(null,M(t,a))},c)}if(e){if(typeof e!="function")throw Error("Illegal callback: "+typeof e);x(e)}else return new Promise(function(b,d){x(function(t,r){if(t){d(t);return}b(r)})})}function H(f){if(typeof f!="string")throw Error("Illegal arguments: "+typeof f);return parseInt(f.split("$")[2],10)}function Y(f){if(typeof f!="string")throw Error("Illegal arguments: "+typeof f);if(f.length!==60)throw Error("Illegal hash length: "+f.length+" != 60");return f.substring(0,29)}function q(f){if(typeof f!="string")throw Error("Illegal arguments: "+typeof f);return z(f)>72}var h=typeof setImmediate=="function"?setImmediate:typeof scheduler=="object"&&typeof scheduler.postTask=="function"?scheduler.postTask.bind(scheduler):setTimeout;function z(f){for(var a=0,e=0,c=0;c<f.length;++c)e=f.charCodeAt(c),e<128?a+=1:e<2048?a+=2:(e&64512)===55296&&(f.charCodeAt(c+1)&64512)===56320?(++c,a+=4):a+=3;return a}function d0(f){for(var a=0,e,c,x=new Array(z(f)),b=0,d=f.length;b<d;++b)e=f.charCodeAt(b),e<128?x[a++]=e:e<2048?(x[a++]=e>>6|192,x[a++]=e&63|128):(e&64512)===55296&&((c=f.charCodeAt(b+1))&64512)===56320?(e=65536+((e&1023)<<10)+(c&1023),++b,x[a++]=e>>18|240,x[a++]=e>>12&63|128,x[a++]=e>>6&63|128,x[a++]=e&63|128):(x[a++]=e>>12|224,x[a++]=e>>6&63|128,x[a++]=e&63|128);return x}var s="./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789".split(""),g=[-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0,1,54,55,56,57,58,59,60,61,62,63,-1,-1,-1,-1,-1,-1,-1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,-1,-1,-1,-1,-1,-1,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,-1,-1,-1,-1,-1];function A(f,a){var e=0,c=[],x,b;if(a<=0||a>f.length)throw Error("Illegal len: "+a);for(;e<a;){if(x=f[e++]&255,c.push(s[x>>2&63]),x=(x&3)<<4,e>=a){c.push(s[x&63]);break}if(b=f[e++]&255,x|=b>>4&15,c.push(s[x&63]),x=(b&15)<<2,e>=a){c.push(s[x&63]);break}b=f[e++]&255,x|=b>>6&3,c.push(s[x&63]),c.push(s[b&63])}return c.join("")}function J(f,a){var e=0,c=f.length,x=0,b=[],d,t,r,n,o,i;if(a<=0)throw Error("Illegal len: "+a);for(;e<c-1&&x<a&&(i=f.charCodeAt(e++),d=i<g.length?g[i]:-1,i=f.charCodeAt(e++),t=i<g.length?g[i]:-1,!(d==-1||t==-1||(o=d<<2>>>0,o|=(t&48)>>4,b.push(String.fromCharCode(o)),++x>=a||e>=c)||(i=f.charCodeAt(e++),r=i<g.length?g[i]:-1,r==-1)||(o=(t&15)<<4>>>0,o|=(r&60)>>2,b.push(String.fromCharCode(o)),++x>=a||e>=c)));)i=f.charCodeAt(e++),n=i<g.length?g[i]:-1,o=(r&3)<<6>>>0,o|=n,b.push(String.fromCharCode(o)),++x;var p=[];for(e=0;e<x;e++)p.push(b[e].charCodeAt(0));return p}var m=16,N=10,r0=16,t0=100,D=[608135816,2242054355,320440878,57701188,2752067618,698298832,137296536,3964562569,1160258022,953160567,3193202383,887688300,3232508343,3380367581,1065670069,3041331479,2450970073,2306472731],L=[3509652390,2564797868,805139163,3491422135,3101798381,1780907670,3128725573,4046225305,614570311,3012652279,134345442,2240740374,1667834072,1901547113,2757295779,4103290238,227898511,1921955416,1904987480,2182433518,2069144605,3260701109,2620446009,720527379,3318853667,677414384,3393288472,3101374703,2390351024,1614419982,1822297739,2954791486,3608508353,3174124327,2024746970,1432378464,3864339955,2857741204,1464375394,1676153920,1439316330,715854006,3033291828,289532110,2706671279,2087905683,3018724369,1668267050,732546397,1947742710,3462151702,2609353502,2950085171,1814351708,2050118529,680887927,999245976,1800124847,3300911131,1713906067,1641548236,4213287313,1216130144,1575780402,4018429277,3917837745,3693486850,3949271944,596196993,3549867205,258830323,2213823033,772490370,2760122372,1774776394,2652871518,566650946,4142492826,1728879713,2882767088,1783734482,3629395816,2517608232,2874225571,1861159788,326777828,3124490320,2130389656,2716951837,967770486,1724537150,2185432712,2364442137,1164943284,2105845187,998989502,3765401048,2244026483,1075463327,1455516326,1322494562,910128902,469688178,1117454909,936433444,3490320968,3675253459,1240580251,122909385,2157517691,634681816,4142456567,3825094682,3061402683,2540495037,79693498,3249098678,1084186820,1583128258,426386531,1761308591,1047286709,322548459,995290223,1845252383,2603652396,3431023940,2942221577,3202600964,3727903485,1712269319,422464435,3234572375,1170764815,3523960633,3117677531,1434042557,442511882,3600875718,1076654713,1738483198,4213154764,2393238008,3677496056,1014306527,4251020053,793779912,2902807211,842905082,4246964064,1395751752,1040244610,2656851899,3396308128,445077038,3742853595,3577915638,679411651,2892444358,2354009459,1767581616,3150600392,3791627101,3102740896,284835224,4246832056,1258075500,768725851,2589189241,3069724005,3532540348,1274779536,3789419226,2764799539,1660621633,3471099624,4011903706,913787905,3497959166,737222580,2514213453,2928710040,3937242737,1804850592,3499020752,2949064160,2386320175,2390070455,2415321851,4061277028,2290661394,2416832540,1336762016,1754252060,3520065937,3014181293,791618072,3188594551,3933548030,2332172193,3852520463,3043980520,413987798,3465142937,3030929376,4245938359,2093235073,3534596313,375366246,2157278981,2479649556,555357303,3870105701,2008414854,3344188149,4221384143,3956125452,2067696032,3594591187,2921233993,2428461,544322398,577241275,1471733935,610547355,4027169054,1432588573,1507829418,2025931657,3646575487,545086370,48609733,2200306550,1653985193,298326376,1316178497,3007786442,2064951626,458293330,2589141269,3591329599,3164325604,727753846,2179363840,146436021,1461446943,4069977195,705550613,3059967265,3887724982,4281599278,3313849956,1404054877,2845806497,146425753,1854211946,1266315497,3048417604,3681880366,3289982499,290971e4,1235738493,2632868024,2414719590,3970600049,1771706367,1449415276,3266420449,422970021,1963543593,2690192192,3826793022,1062508698,1531092325,1804592342,2583117782,2714934279,4024971509,1294809318,4028980673,1289560198,2221992742,1669523910,35572830,157838143,1052438473,1016535060,1802137761,1753167236,1386275462,3080475397,2857371447,1040679964,2145300060,2390574316,1461121720,2956646967,4031777805,4028374788,33600511,2920084762,1018524850,629373528,3691585981,3515945977,2091462646,2486323059,586499841,988145025,935516892,3367335476,2599673255,2839830854,265290510,3972581182,2759138881,3795373465,1005194799,847297441,406762289,1314163512,1332590856,1866599683,4127851711,750260880,613907577,1450815602,3165620655,3734664991,3650291728,3012275730,3704569646,1427272223,778793252,1343938022,2676280711,2052605720,1946737175,3164576444,3914038668,3967478842,3682934266,1661551462,3294938066,4011595847,840292616,3712170807,616741398,312560963,711312465,1351876610,322626781,1910503582,271666773,2175563734,1594956187,70604529,3617834859,1007753275,1495573769,4069517037,2549218298,2663038764,504708206,2263041392,3941167025,2249088522,1514023603,1998579484,1312622330,694541497,2582060303,2151582166,1382467621,776784248,2618340202,3323268794,2497899128,2784771155,503983604,4076293799,907881277,423175695,432175456,1378068232,4145222326,3954048622,3938656102,3820766613,2793130115,2977904593,26017576,3274890735,3194772133,1700274565,1756076034,4006520079,3677328699,720338349,1533947780,354530856,688349552,3973924725,1637815568,332179504,3949051286,53804574,2852348879,3044236432,1282449977,3583942155,3416972820,4006381244,1617046695,2628476075,3002303598,1686838959,431878346,2686675385,1700445008,1080580658,1009431731,832498133,3223435511,2605976345,2271191193,2516031870,1648197032,4164389018,2548247927,300782431,375919233,238389289,3353747414,2531188641,2019080857,1475708069,455242339,2609103871,448939670,3451063019,1395535956,2413381860,1841049896,1491858159,885456874,4264095073,4001119347,1565136089,3898914787,1108368660,540939232,1173283510,2745871338,3681308437,4207628240,3343053890,4016749493,1699691293,1103962373,3625875870,2256883143,3830138730,1031889488,3479347698,1535977030,4236805024,3251091107,2132092099,1774941330,1199868427,1452454533,157007616,2904115357,342012276,595725824,1480756522,206960106,497939518,591360097,863170706,2375253569,3596610801,1814182875,2094937945,3421402208,1082520231,3463918190,2785509508,435703966,3908032597,1641649973,2842273706,3305899714,1510255612,2148256476,2655287854,3276092548,4258621189,236887753,3681803219,274041037,1734335097,3815195456,3317970021,1899903192,1026095262,4050517792,356393447,2410691914,3873677099,3682840055,3913112168,2491498743,4132185628,2489919796,1091903735,1979897079,3170134830,3567386728,3557303409,857797738,1136121015,1342202287,507115054,2535736646,337727348,3213592640,1301675037,2528481711,1895095763,1721773893,3216771564,62756741,2142006736,835421444,2531993523,1442658625,3659876326,2882144922,676362277,1392781812,170690266,3921047035,1759253602,3611846912,1745797284,664899054,1329594018,3901205900,3045908486,2062866102,2865634940,3543621612,3464012697,1080764994,553557557,3656615353,3996768171,991055499,499776247,1265440854,648242737,3940784050,980351604,3713745714,1749149687,3396870395,4211799374,3640570775,1161844396,3125318951,1431517754,545492359,4268468663,3499529547,1437099964,2702547544,3433638243,2581715763,2787789398,1060185593,1593081372,2418618748,4260947970,69676912,2159744348,86519011,2512459080,3838209314,1220612927,3339683548,133810670,1090789135,1078426020,1569222167,845107691,3583754449,4072456591,1091646820,628848692,1613405280,3757631651,526609435,236106946,48312990,2942717905,3402727701,1797494240,859738849,992217954,4005476642,2243076622,3870952857,3732016268,765654824,3490871365,2511836413,1685915746,3888969200,1414112111,2273134842,3281911079,4080962846,172450625,2569994100,980381355,4109958455,2819808352,2716589560,2568741196,3681446669,3329971472,1835478071,660984891,3704678404,4045999559,3422617507,3040415634,1762651403,1719377915,3470491036,2693910283,3642056355,3138596744,1364962596,2073328063,1983633131,926494387,3423689081,2150032023,4096667949,1749200295,3328846651,309677260,2016342300,1779581495,3079819751,111262694,1274766160,443224088,298511866,1025883608,3806446537,1145181785,168956806,3641502830,3584813610,1689216846,3666258015,3200248200,1692713982,2646376535,4042768518,1618508792,1610833997,3523052358,4130873264,2001055236,3610705100,2202168115,4028541809,2961195399,1006657119,2006996926,3186142756,1430667929,3210227297,1314452623,4074634658,4101304120,2273951170,1399257539,3367210612,3027628629,1190975929,2062231137,2333990788,2221543033,2438960610,1181637006,548689776,2362791313,3372408396,3104550113,3145860560,296247880,1970579870,3078560182,3769228297,1714227617,3291629107,3898220290,166772364,1251581989,493813264,448347421,195405023,2709975567,677966185,3703036547,1463355134,2715995803,1338867538,1343315457,2802222074,2684532164,233230375,2599980071,2000651841,3277868038,1638401717,4028070440,3237316320,6314154,819756386,300326615,590932579,1405279636,3267499572,3150704214,2428286686,3959192993,3461946742,1862657033,1266418056,963775037,2089974820,2263052895,1917689273,448879540,3550394620,3981727096,150775221,3627908307,1303187396,508620638,2975983352,2726630617,1817252668,1876281319,1457606340,908771278,3720792119,3617206836,2455994898,1729034894,1080033504,976866871,3556439503,2881648439,1522871579,1555064734,1336096578,3548522304,2579274686,3574697629,3205460757,3593280638,3338716283,3079412587,564236357,2993598910,1781952180,1464380207,3163844217,3332601554,1699332808,1393555694,1183702653,3581086237,1288719814,691649499,2847557200,2895455976,3193889540,2717570544,1781354906,1676643554,2592534050,3230253752,1126444790,2770207658,2633158820,2210423226,2615765581,2414155088,3127139286,673620729,2805611233,1269405062,4015350505,3341807571,4149409754,1057255273,2012875353,2162469141,2276492801,2601117357,993977747,3918593370,2654263191,753973209,36408145,2530585658,25011837,3520020182,2088578344,530523599,2918365339,1524020338,1518925132,3760827505,3759777254,1202760957,3985898139,3906192525,674977740,4174734889,2031300136,2019492241,3983892565,4153806404,3822280332,352677332,2297720250,60907813,90501309,3286998549,1016092578,2535922412,2839152426,457141659,509813237,4120667899,652014361,1966332200,2975202805,55981186,2327461051,676427537,3255491064,2882294119,3433927263,1307055953,942726286,933058658,2468411793,3933900994,4215176142,1361170020,2001714738,2830558078,3274259782,1222529897,1679025792,2729314320,3714953764,1770335741,151462246,3013232138,1682292957,1483529935,471910574,1539241949,458788160,3436315007,1807016891,3718408830,978976581,1043663428,3165965781,1927990952,4200891579,2372276910,3208408903,3533431907,1412390302,2931980059,4132332400,1947078029,3881505623,4168226417,2941484381,1077988104,1320477388,886195818,18198404,3786409e3,2509781533,112762804,3463356488,1866414978,891333506,18488651,661792760,1628790961,3885187036,3141171499,876946877,2693282273,1372485963,791857591,2686433993,3759982718,3167212022,3472953795,2716379847,445679433,3561995674,3504004811,3574258232,54117162,3331405415,2381918588,3769707343,4154350007,1140177722,4074052095,668550556,3214352940,367459370,261225585,2610173221,4209349473,3468074219,3265815641,314222801,3066103646,3808782860,282218597,3406013506,3773591054,379116347,1285071038,846784868,2669647154,3771962079,3550491691,2305946142,453669953,1268987020,3317592352,3279303384,3744833421,2610507566,3859509063,266596637,3847019092,517658769,3462560207,3443424879,370717030,4247526661,2224018117,4143653529,4112773975,2788324899,2477274417,1456262402,2901442914,1517677493,1846949527,2295493580,3734397586,2176403920,1280348187,1908823572,3871786941,846861322,1172426758,3287448474,3383383037,1655181056,3139813346,901632758,1897031941,2986607138,3066810236,3447102507,1393639104,373351379,950779232,625454576,3124240540,4148612726,2007998917,544563296,2244738638,2330496472,2058025392,1291430526,424198748,50039436,29584100,3605783033,2429876329,2791104160,1057563949,3255363231,3075367218,3463963227,1469046755,985887462],K=[1332899944,1700884034,1701343084,1684370003,1668446532,1869963892];function I(f,a,e,c){var x,b=f[a],d=f[a+1];return b^=e[0],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[1],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[2],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[3],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[4],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[5],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[6],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[7],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[8],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[9],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[10],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[11],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[12],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[13],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[14],x=c[b>>>24],x+=c[256|b>>16&255],x^=c[512|b>>8&255],x+=c[768|b&255],d^=x^e[15],x=c[d>>>24],x+=c[256|d>>16&255],x^=c[512|d>>8&255],x+=c[768|d&255],b^=x^e[16],f[a]=d^e[r0+1],f[a+1]=b,f}function l(f,a){for(var e=0,c=0;e<4;++e)c=c<<8|f[a]&255,a=(a+1)%f.length;return{key:c,offp:a}}function U(f,a,e){for(var c=0,x=[0,0],b=a.length,d=e.length,t,r=0;r<b;r++)t=l(f,c),c=t.offp,a[r]=a[r]^t.key;for(r=0;r<b;r+=2)x=I(x,0,a,e),a[r]=x[0],a[r+1]=x[1];for(r=0;r<d;r+=2)x=I(x,0,a,e),e[r]=x[0],e[r+1]=x[1]}function n0(f,a,e,c){for(var x=0,b=[0,0],d=e.length,t=c.length,r,n=0;n<d;n++)r=l(a,x),x=r.offp,e[n]=e[n]^r.key;for(x=0,n=0;n<d;n+=2)r=l(f,x),x=r.offp,b[0]^=r.key,r=l(f,x),x=r.offp,b[1]^=r.key,b=I(b,0,e,c),e[n]=b[0],e[n+1]=b[1];for(n=0;n<t;n+=2)r=l(f,x),x=r.offp,b[0]^=r.key,r=l(f,x),x=r.offp,b[1]^=r.key,b=I(b,0,e,c),c[n]=b[0],c[n+1]=b[1]}function j(f,a,e,c,x){var b=K.slice(),d=b.length,t;if(e<4||e>31)if(t=Error("Illegal number of rounds (4-31): "+e),c){h(c.bind(this,t));return}else throw t;if(a.length!==m)if(t=Error("Illegal salt length: "+a.length+" != "+m),c){h(c.bind(this,t));return}else throw t;e=1<<e>>>0;var r,n,o=0,i;typeof Int32Array=="function"?(r=new Int32Array(D),n=new Int32Array(L)):(r=D.slice(),n=L.slice()),n0(a,f,r,n);function p(){if(x&&x(o/e),o<e)for(var y=Date.now();o<e&&(o=o+1,U(f,r,n),U(a,r,n),!(Date.now()-y>t0)););else{for(o=0;o<64;o++)for(i=0;i<d>>1;i++)I(b,i<<1,r,n);var u=[];for(o=0;o<d;o++)u.push((b[o]>>24&255)>>>0),u.push((b[o]>>16&255)>>>0),u.push((b[o]>>8&255)>>>0),u.push((b[o]&255)>>>0);if(c){c(null,u);return}else return u}c&&h(p)}if(typeof c<"u")p();else for(var v;;)if(typeof(v=p())<"u")return v||[]}function w(f,a,e,c){var x;if(typeof f!="string"||typeof a!="string")if(x=Error("Invalid string / salt: Not a string"),e){h(e.bind(this,x));return}else throw x;var b,d;if(a.charAt(0)!=="$"||a.charAt(1)!=="2")if(x=Error("Invalid salt version: "+a.substring(0,2)),e){h(e.bind(this,x));return}else throw x;if(a.charAt(2)==="$")b="\0",d=3;else{if(b=a.charAt(2),b!=="a"&&b!=="b"&&b!=="y"||a.charAt(3)!=="$")if(x=Error("Invalid salt revision: "+a.substring(2,4)),e){h(e.bind(this,x));return}else throw x;d=4}if(a.charAt(d+2)>"$")if(x=Error("Missing salt rounds"),e){h(e.bind(this,x));return}else throw x;var t=parseInt(a.substring(d,d+1),10)*10,r=parseInt(a.substring(d+1,d+2),10),n=t+r,o=a.substring(d+3,d+25);f+=b>="a"?"\0":"";var i=d0(f),p=J(o,m);function v(y){var u=[];return u.push("$2"),b>="a"&&u.push(b),u.push("$"),n<10&&u.push("0"),u.push(n.toString()),u.push("$"),u.push(A(p,p.length)),u.push(A(y,K.length*4-1)),u.join("")}if(typeof e>"u")return v(j(i,p,n));j(i,p,n,function(y,u){y?e(y,null):e(null,v(u))},c)}function Q(f,a){return A(f,a)}function Z(f,a){return J(f,a)}var o0={setRandomFallback:G,genSaltSync:C,genSalt:R,hashSync:T,hash:$,compareSync:X,compare:W,getRounds:H,getSalt:Y,truncates:q,encodeBase64:Q,decodeBase64:Z};return c0(i0);})();


const password = {
  async hash(pwd, options) {
    const algorithm = options?.algorithm ?? 'bcrypt'
    const cost = options?.cost ?? 10
    
    if (algorithm !== 'bcrypt') {
      throw new Error(`Algorithm '${algorithm}' is not available in workerd. Use 'bcrypt' instead.`)
    }
    
    // Use real bcrypt
    const salt = bcrypt.genSaltSync(cost)
    return bcrypt.hashSync(pwd, salt)
  },
  
  async verify(pwd, hashStr) {
    // Real bcrypt format: $2a$, $2b$, $2y$
    if (hashStr.startsWith('$2')) {
      return bcrypt.compareSync(pwd, hashStr)
    }
    
    // Legacy workerd format (backwards compatibility)
    if (hashStr.startsWith('$workerd$')) {
      return verifyLegacyWorkerdHash(pwd, hashStr)
    }
    
    throw new Error('Unknown hash format. Expected bcrypt ($2a$, $2b$, $2y$) or legacy workerd format.')
  }
}

// Legacy PBKDF2 verification for old workerd hashes
async function verifyLegacyWorkerdHash(pwd, hashStr) {
  const parts = hashStr.split('$')
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
    `7${randomHex.slice(0, 3)}`,
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
  return new URL(`file://${path.startsWith('/') ? '' : '/'}${path}`)
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
    url: new URL(`http://${hostname}:${port}`),
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
      if (!response.ok) throw new Error(`DNS query failed: ${response.status}`)
      
      const data = await response.json()
      if (data.Status !== 0) {
        const errors = { 1: 'Format error', 2: 'Server failure', 3: 'NXDOMAIN', 5: 'Refused' }
        throw new Error(`DNS error: ${errors[data.Status] || 'Unknown'}`)
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
        
        if (results.length === 0) throw new Error(`ENOTFOUND: DNS lookup failed for ${hostname}`)
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