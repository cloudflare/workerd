var __defProp = Object.defineProperty;
var __require = /* @__PURE__ */ ((x) => typeof require !== "undefined" ? require : typeof Proxy !== "undefined" ? new Proxy(x, {
  get: (a, b) => (typeof require !== "undefined" ? require : a)[b]
}) : x)(function(x) {
  if (typeof require !== "undefined") return require.apply(this, arguments);
  throw Error('Dynamic require of "' + x + '" is not supported');
});
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};

// bun.ts
import { ERR_FS_FILE_NOT_FOUND } from "bun-internal:errors";
import { isArrayBuffer, isString, isUint8Array } from "bun-internal:types";

// dns.ts
var dns_exports = {};
__export(dns_exports, {
  default: () => dns_default,
  getProvider: () => getProvider,
  getServers: () => getServers,
  lookup: () => lookup,
  resetServers: () => resetServers,
  resolve: () => resolve,
  resolve4: () => resolve4,
  resolve6: () => resolve6,
  resolveCname: () => resolveCname,
  resolveMx: () => resolveMx,
  resolveNs: () => resolveNs,
  resolveSrv: () => resolveSrv,
  resolveTxt: () => resolveTxt,
  reverse: () => reverse,
  setProvider: () => setProvider,
  setServers: () => setServers
});
var RECORD_TYPES = {
  A: 1,
  AAAA: 28,
  CNAME: 5,
  MX: 15,
  TXT: 16,
  NS: 2,
  SOA: 6,
  SRV: 33,
  PTR: 12
};
var DOH_PROVIDERS = {
  cloudflare: "https://cloudflare-dns.com/dns-query",
  google: "https://dns.google/resolve"
};
var currentProvider = "cloudflare";
var customDoHEndpoint = null;
function setProvider(provider) {
  if (!DOH_PROVIDERS[provider]) {
    throw new Error(
      `Unknown DNS provider: ${provider}. Use 'cloudflare' or 'google'.`
    );
  }
  currentProvider = provider;
}
function getProvider() {
  return currentProvider;
}
async function doQuery(hostname, type) {
  const baseUrl = customDoHEndpoint ?? DOH_PROVIDERS[currentProvider];
  const url = new URL(baseUrl);
  const isCloudflareStyle = customDoHEndpoint === null ? currentProvider === "cloudflare" : baseUrl.includes("cloudflare");
  url.searchParams.set("name", hostname);
  if (isCloudflareStyle) {
    url.searchParams.set("type", type);
  } else {
    url.searchParams.set("type", String(RECORD_TYPES[type]));
  }
  const response = await fetch(url.toString(), {
    headers: {
      Accept: "application/dns-json"
    }
  });
  if (!response.ok) {
    throw new Error(
      `DNS query failed: ${response.status} ${response.statusText}`
    );
  }
  const data = await response.json();
  if (data.Status !== 0) {
    const errorCodes = {
      1: "Format error",
      2: "Server failure",
      3: "NXDOMAIN - domain does not exist",
      4: "Not implemented",
      5: "Refused"
    };
    throw new Error(
      `DNS error: ${errorCodes[data.Status] ?? `Unknown error (${data.Status})`}`
    );
  }
  return data.Answer ?? [];
}
async function lookup(hostname, options) {
  const family = options?.family ?? 0;
  const all = options?.all ?? false;
  const results = [];
  let lastError = null;
  if (family === 0 || family === 4) {
    try {
      const answers = await doQuery(hostname, "A");
      for (const answer of answers) {
        if (answer.type === RECORD_TYPES.A) {
          results.push({ address: answer.data, family: 4 });
        }
      }
    } catch (err) {
      if (family === 4) {
        throw err;
      }
      lastError = err instanceof Error ? err : new Error(String(err));
    }
  }
  if (family === 0 || family === 6) {
    try {
      const answers = await doQuery(hostname, "AAAA");
      for (const answer of answers) {
        if (answer.type === RECORD_TYPES.AAAA) {
          results.push({ address: answer.data, family: 6 });
        }
      }
    } catch (err) {
      if (family === 6) {
        throw err;
      }
      if (lastError === null) {
        lastError = err instanceof Error ? err : new Error(String(err));
      }
    }
  }
  if (results.length === 0) {
    if (lastError !== null) {
      throw lastError;
    }
    throw new Error(`ENOTFOUND: DNS lookup failed for ${hostname}`);
  }
  if (all) {
    return results;
  }
  return results[0].address;
}
async function resolve(hostname, rrtype = "A") {
  const answers = await doQuery(hostname, rrtype);
  return answers.filter((a) => a.type === RECORD_TYPES[rrtype]).map((a) => a.data);
}
async function resolve4(hostname) {
  return resolve(hostname, "A");
}
async function resolve6(hostname) {
  return resolve(hostname, "AAAA");
}
async function resolveCname(hostname) {
  return resolve(hostname, "CNAME");
}
async function resolveMx(hostname) {
  const answers = await doQuery(hostname, "MX");
  return answers.filter((a) => a.type === RECORD_TYPES.MX).map((a) => {
    const parts = a.data.split(" ");
    const priorityStr = parts[0] ?? "0";
    return {
      priority: parseInt(priorityStr, 10),
      exchange: parts.slice(1).join(" ")
    };
  }).sort((a, b) => a.priority - b.priority);
}
async function resolveTxt(hostname) {
  const answers = await doQuery(hostname, "TXT");
  return answers.filter((a) => a.type === RECORD_TYPES.TXT).map((a) => [a.data.replace(/^"|"$/g, "")]);
}
async function resolveNs(hostname) {
  return resolve(hostname, "NS");
}
async function resolveSrv(hostname) {
  const answers = await doQuery(hostname, "SRV");
  return answers.filter((a) => a.type === RECORD_TYPES.SRV).map((a) => {
    const parts = a.data.split(" ");
    return {
      priority: parseInt(parts[0] ?? "0", 10),
      weight: parseInt(parts[1] ?? "0", 10),
      port: parseInt(parts[2] ?? "0", 10),
      name: parts[3] ?? ""
    };
  });
}
async function reverse(ip) {
  let reverseAddr;
  if (ip.includes(":")) {
    const expanded = ip.split(":").map((part) => part.padStart(4, "0")).join("");
    reverseAddr = `${expanded.split("").reverse().join(".")}.ip6.arpa`;
  } else {
    reverseAddr = `${ip.split(".").reverse().join(".")}.in-addr.arpa`;
  }
  return resolve(reverseAddr, "PTR");
}
function getServers() {
  if (customDoHEndpoint !== null) {
    return [customDoHEndpoint];
  }
  return [DOH_PROVIDERS[currentProvider]];
}
function setServers(servers) {
  if (servers.length === 0) return;
  const server = servers[0];
  if (server === void 0) return;
  if (server.startsWith("https://")) {
    if (server.includes("cloudflare")) {
      currentProvider = "cloudflare";
      customDoHEndpoint = null;
    } else if (server.includes("google")) {
      currentProvider = "google";
      customDoHEndpoint = null;
    } else {
      customDoHEndpoint = server;
    }
  } else if (server.match(/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/)) {
    console.warn(
      `[dns.setServers] IP address "${server}" cannot be used with DNS-over-HTTPS. Provide a DoH endpoint URL (https://...) or use a known provider.`
    );
  } else if (server.startsWith("http://")) {
    console.warn(
      `[dns.setServers] Non-HTTPS URL "${server}" is not supported. DNS-over-HTTPS requires a secure HTTPS endpoint.`
    );
  } else {
    console.warn(
      `[dns.setServers] Unrecognized server format: "${server}". Expected an HTTPS URL like "https://cloudflare-dns.com/dns-query".`
    );
  }
}
function resetServers() {
  currentProvider = "cloudflare";
  customDoHEndpoint = null;
}
var dns_default = {
  lookup,
  resolve,
  resolve4,
  resolve6,
  resolveCname,
  resolveMx,
  resolveTxt,
  resolveNs,
  resolveSrv,
  reverse,
  getServers,
  setServers,
  resetServers,
  setProvider,
  getProvider
};

// bun.ts
var fsModule = null;
var fsPromisesModule = null;
var streamModule = null;
function getFs() {
  if (!fsModule) {
    try {
      fsModule = __require("node:fs");
    } catch {
      throw new Error(
        'File operations require node:fs.\nEnable with: compatibilityFlags = ["nodejs_compat", "enable_nodejs_fs_module"]'
      );
    }
  }
  return fsModule;
}
function getFsPromises() {
  if (!fsPromisesModule) {
    try {
      fsPromisesModule = __require("node:fs/promises");
    } catch {
      throw new Error(
        'File operations require node:fs/promises.\nEnable with: compatibilityFlags = ["nodejs_compat", "enable_nodejs_fs_module"]'
      );
    }
  }
  return fsPromisesModule;
}
function getStream() {
  if (!streamModule) {
    try {
      streamModule = __require("node:stream");
    } catch {
      throw new Error("Stream operations require node:stream.");
    }
  }
  return streamModule;
}
var BunFileImpl = class _BunFileImpl {
  path;
  _type;
  sliceStart;
  sliceEnd;
  constructor(path, options, sliceStart, sliceEnd) {
    this.path = typeof path === "string" ? path : path.pathname;
    this._type = options?.type ?? "application/octet-stream";
    this.sliceStart = sliceStart;
    this.sliceEnd = sliceEnd;
  }
  get size() {
    const fs = getFs();
    if (!fs.existsSync(this.path)) return 0;
    const st = fs.statSync(this.path);
    const len = st.size;
    if (this.sliceStart === void 0 && this.sliceEnd === void 0) return len;
    const start = this.sliceStart ?? 0;
    const end = this.sliceEnd ?? len;
    return Math.max(0, end - start);
  }
  get type() {
    return this._type;
  }
  get name() {
    return this.path;
  }
  get lastModified() {
    const fs = getFs();
    if (!fs.existsSync(this.path)) return Date.now();
    const st = fs.statSync(this.path);
    return st.mtimeMs;
  }
  async text() {
    const data = await this.bytes();
    return new TextDecoder().decode(data);
  }
  async json() {
    return JSON.parse(await this.text());
  }
  async arrayBuffer() {
    const data = await this.bytes();
    const copy = new ArrayBuffer(data.byteLength);
    new Uint8Array(copy).set(data);
    return copy;
  }
  async bytes() {
    const fs = getFs();
    const fsp = getFsPromises();
    if (!fs.existsSync(this.path)) throw new ERR_FS_FILE_NOT_FOUND(this.path);
    const buf = await fsp.readFile(this.path);
    const bytes = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
    if (this.sliceStart === void 0 && this.sliceEnd === void 0)
      return bytes;
    return bytes.slice(this.sliceStart, this.sliceEnd);
  }
  stream() {
    const fs = getFs();
    const { Readable } = getStream();
    if (!fs.existsSync(this.path)) throw new ERR_FS_FILE_NOT_FOUND(this.path);
    const start = this.sliceStart;
    const end = this.sliceEnd === void 0 ? void 0 : Math.max(0, this.sliceEnd - 1);
    const nodeStream = fs.createReadStream(this.path, {
      start,
      end
    });
    return Readable.toWeb(nodeStream);
  }
  slice(start, end, type) {
    if (this.sliceStart !== void 0 || this.sliceEnd !== void 0) {
      const baseStart = this.sliceStart ?? 0;
      const newStart = start === void 0 ? this.sliceStart : baseStart + start;
      const newEnd = end === void 0 ? this.sliceEnd : this.sliceStart === void 0 ? end : baseStart + end;
      return new _BunFileImpl(
        this.path,
        { type: type ?? this._type },
        newStart,
        newEnd
      );
    }
    return new _BunFileImpl(this.path, { type: type ?? this._type }, start, end);
  }
  async exists() {
    const fs = getFs();
    return fs.existsSync(this.path);
  }
  writer() {
    const fs = getFs();
    const fd = fs.openSync(this.path, "w");
    let closed = false;
    return {
      write(data) {
        if (closed) {
          throw new Error("FileSink is closed");
        }
        const bytes = typeof data === "string" ? new TextEncoder().encode(data) : data instanceof ArrayBuffer ? new Uint8Array(data) : data;
        return fs.writeSync(fd, bytes, 0, bytes.byteLength);
      },
      flush() {
      },
      // writeSync is unbuffered
      end() {
        if (closed) return;
        fs.closeSync(fd);
        closed = true;
      }
    };
  }
};
function file(path, options) {
  return new BunFileImpl(path, options);
}
async function write(destination, data) {
  const path = isString(destination) ? destination : destination instanceof URL ? destination.pathname : destination.name;
  let bytes;
  if (isString(data)) {
    bytes = new TextEncoder().encode(data);
  } else if (isArrayBuffer(data)) {
    bytes = new Uint8Array(data);
  } else if (isUint8Array(data)) {
    bytes = data;
  } else if (data instanceof Blob) {
    bytes = new Uint8Array(await data.arrayBuffer());
  } else if (data instanceof Response) {
    bytes = new Uint8Array(await data.arrayBuffer());
  } else {
    bytes = await data.bytes();
  }
  const fsp = getFsPromises();
  await fsp.writeFile(path, bytes);
  return bytes.byteLength;
}
var currentServeOptions = null;
function serve(options) {
  const isWorkerd = typeof navigator !== "undefined" && navigator.userAgent?.includes("Cloudflare-Workers");
  if (isWorkerd && !options.development) {
    throw new Error(
      'Bun.serve() does not create a real HTTP server in workerd.\nFor production, use the native pattern:\n  export default { fetch(request) { return new Response("OK") } }\n\nFor testing, pass development: true to use this as a test helper.'
    );
  }
  currentServeOptions = options;
  const port = options.port ?? 3e3;
  const hostname = options.hostname ?? "localhost";
  return {
    port,
    hostname,
    development: options.development ?? false,
    url: new URL(`http://${hostname}:${port}`),
    stop() {
      currentServeOptions = null;
    },
    ref() {
    },
    unref() {
    },
    reload(newOptions) {
      currentServeOptions = { ...currentServeOptions, ...newOptions };
    },
    async fetch(request) {
      if (!currentServeOptions) throw new Error("Server is not running");
      try {
        return await currentServeOptions.fetch(request);
      } catch (error) {
        if (currentServeOptions.error && error instanceof Error) {
          return currentServeOptions.error(error);
        }
        throw error;
      }
    }
  };
}
function getServeHandler() {
  return currentServeOptions;
}
var getProcess = () => globalThis.process;
var env = new Proxy(
  {},
  {
    get(_, prop) {
      return getProcess()?.env?.[prop];
    },
    set(_, prop, value) {
      const proc = getProcess();
      if (proc?.env) proc.env[prop] = value;
      return true;
    },
    has(_, prop) {
      return prop in (getProcess()?.env ?? {});
    },
    ownKeys() {
      return Object.keys(getProcess()?.env ?? {});
    },
    getOwnPropertyDescriptor(_, prop) {
      const env2 = getProcess()?.env;
      if (env2 && prop in env2) {
        return { enumerable: true, configurable: true, value: env2[prop] };
      }
      return void 0;
    }
  }
);
var version = "1.0.0-workerd";
var revision = "workerd-compat";
function sleep(ms) {
  return new Promise((resolve2) => setTimeout(resolve2, ms));
}
function sleepSync(ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
  }
}
function nanoseconds() {
  return BigInt(Math.floor(performance.now() * 1e6));
}
function escapeHTML(str) {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;");
}
function stringWidth(str) {
  let width = 0;
  for (const char of str) {
    const code = char.codePointAt(0);
    const isWide = code >= 4352 && code <= 4447 || code >= 11904 && code <= 42191 || code >= 44032 && code <= 55203 || code >= 63744 && code <= 64255 || code >= 65040 && code <= 65055 || code >= 65072 && code <= 65135 || code >= 65280 && code <= 65376 || code >= 65504 && code <= 65510 || code >= 131072 && code <= 196605 || code >= 196608 && code <= 262141;
    width += isWide ? 2 : 1;
  }
  return width;
}
function deepEquals(a, b) {
  if (a === b) return true;
  if (typeof a !== typeof b) return false;
  if (a === null || b === null) return a === b;
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) return false;
    return a.every((item, index) => deepEquals(item, b[index]));
  }
  if (typeof a === "object" && typeof b === "object") {
    const aObj = a;
    const bObj = b;
    const aKeys = Object.keys(aObj);
    const bKeys = Object.keys(bObj);
    if (aKeys.length !== bKeys.length) return false;
    return aKeys.every((key) => key in bObj && deepEquals(aObj[key], bObj[key]));
  }
  return false;
}
function inspect(obj, options) {
  const depth = options?.depth ?? 2;
  return inspectValue(obj, depth, /* @__PURE__ */ new Set());
}
function inspectValue(value, depth, seen) {
  if (value === null) return "null";
  if (value === void 0) return "undefined";
  const type = typeof value;
  if (type === "string") return JSON.stringify(value);
  if (type === "number" || type === "boolean" || type === "bigint")
    return String(value);
  if (type === "function")
    return `[Function: ${value.name || "anonymous"}]`;
  if (type === "symbol") return value.toString();
  if (seen.has(value)) return "[Circular]";
  if (Array.isArray(value)) {
    if (depth < 0) return "[Array]";
    seen.add(value);
    const items = value.map((item) => inspectValue(item, depth - 1, seen));
    seen.delete(value);
    return `[ ${items.join(", ")} ]`;
  }
  if (value instanceof Date) return value.toISOString();
  if (value instanceof RegExp) return value.toString();
  if (value instanceof Error) return `${value.name}: ${value.message}`;
  if (type === "object") {
    if (depth < 0) return "[Object]";
    seen.add(value);
    const obj = value;
    const entries = Object.entries(obj).map(
      ([k, v]) => `${k}: ${inspectValue(v, depth - 1, seen)}`
    );
    seen.delete(value);
    return `{ ${entries.join(", ")} }`;
  }
  return String(value);
}
function toBytes(data) {
  return isString(data) ? new TextEncoder().encode(data) : isArrayBuffer(data) ? new Uint8Array(data) : data;
}
function hashImpl(data, algorithm) {
  const bytes = toBytes(data);
  switch (algorithm ?? "wyhash") {
    case "wyhash":
      return wyhash(bytes);
    case "crc32":
      return crc32(bytes);
    case "adler32":
      return adler32(bytes);
    case "cityhash32":
      return cityhash32(bytes);
    case "cityhash64":
      return cityhash64(bytes);
    case "murmur32v3":
      return murmur32v3(bytes);
    case "murmur64v2":
      return murmur64v2(bytes);
  }
}
var hash = Object.assign(hashImpl, {
  wyhash: (data, seed) => wyhash(toBytes(data), seed),
  crc32: (data) => crc32(toBytes(data)),
  adler32: (data) => adler32(toBytes(data)),
  cityhash32: (data) => cityhash32(toBytes(data)),
  cityhash64: (data) => cityhash64(toBytes(data)),
  murmur32v3: (data, seed) => murmur32v3(toBytes(data), seed),
  murmur64v2: (data, seed) => murmur64v2(toBytes(data), seed)
});
function wyhash(data, seed = 0) {
  let h = BigInt(seed);
  for (let i = 0; i < data.length; i++) {
    h = (h ^ BigInt(data[i])) * 0x9e3779b97f4a7c15n;
    h = h ^ h >> 32n;
  }
  return h;
}
function crc32(data) {
  let crc = 4294967295;
  for (let i = 0; i < data.length; i++) {
    crc = crc ^ data[i];
    for (let j = 0; j < 8; j++) {
      crc = crc & 1 ? crc >>> 1 ^ 3988292384 : crc >>> 1;
    }
  }
  return (crc ^ 4294967295) >>> 0;
}
function adler32(data) {
  let a = 1, b = 0;
  const mod = 65521;
  for (let i = 0; i < data.length; i++) {
    a = (a + data[i]) % mod;
    b = (b + a) % mod;
  }
  return (b << 16 | a) >>> 0;
}
function cityhash32(data) {
  let h = 2166136261;
  for (let i = 0; i < data.length; i++) {
    h ^= data[i];
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}
function cityhash64(data) {
  let h = 0xcbf29ce484222325n;
  for (let i = 0; i < data.length; i++) {
    h ^= BigInt(data[i]);
    h *= 0x100000001b3n;
  }
  return h;
}
function murmur32v3(data, seed = 0) {
  let h = seed;
  const c1 = 3432918353, c2 = 461845907;
  for (let i = 0; i < data.length; i += 4) {
    let k = data[i] | (data[i + 1] ?? 0) << 8 | (data[i + 2] ?? 0) << 16 | (data[i + 3] ?? 0) << 24;
    k = Math.imul(k, c1);
    k = k << 15 | k >>> 17;
    k = Math.imul(k, c2);
    h ^= k;
    h = h << 13 | h >>> 19;
    h = Math.imul(h, 5) + 3864292196;
  }
  h ^= data.length;
  h ^= h >>> 16;
  h = Math.imul(h, 2246822507);
  h ^= h >>> 13;
  h = Math.imul(h, 3266489909);
  h ^= h >>> 16;
  return h >>> 0;
}
function murmur64v2(data, seed = 0) {
  let h = BigInt(seed) ^ BigInt(data.length) * 0xc6a4a7935bd1e995n;
  const m = 0xc6a4a7935bd1e995n, r = 47n;
  for (let i = 0; i < data.length - 7; i += 8) {
    let k = BigInt(data[i]) | BigInt(data[i + 1]) << 8n | BigInt(data[i + 2]) << 16n | BigInt(data[i + 3]) << 24n | BigInt(data[i + 4]) << 32n | BigInt(data[i + 5]) << 40n | BigInt(data[i + 6]) << 48n | BigInt(data[i + 7]) << 56n;
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
  }
  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}
var PBKDF2_HASH_NAME = "SHA-256";
var PBKDF2_DERIVE_BITS = 256;
var DEFAULT_PBKDF2_COST = 10;
var DEFAULT_PBKDF2_SALT_BYTES = 16;
function bytesToHex(bytes) {
  return Array.from(bytes).map((b) => b.toString(16).padStart(2, "0")).join("");
}
function hexToBytes(hex) {
  if (hex.length % 2 !== 0) throw new Error("Invalid hex string");
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < bytes.length; i++) {
    bytes[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return bytes;
}
function constantTimeEqual(a, b) {
  if (a.length !== b.length) return false;
  let result = 0;
  for (let i = 0; i < a.length; i++) {
    result |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return result === 0;
}
async function pbkdf2Hash(password2, iterations, salt) {
  const passwordData = new TextEncoder().encode(password2);
  const keyMaterial = await crypto.subtle.importKey(
    "raw",
    passwordData,
    "PBKDF2",
    false,
    ["deriveBits"]
  );
  const saltBuffer = new Uint8Array(salt).buffer;
  const derivedBits = await crypto.subtle.deriveBits(
    {
      name: "PBKDF2",
      salt: new Uint8Array(saltBuffer),
      iterations,
      hash: PBKDF2_HASH_NAME
    },
    keyMaterial,
    PBKDF2_DERIVE_BITS
  );
  return bytesToHex(new Uint8Array(derivedBits));
}
var password = {
  async hash(password2, options) {
    const algorithm = options?.algorithm ?? "pbkdf2";
    if (algorithm !== "pbkdf2") {
      throw new Error(
        `Algorithm '${algorithm}' is not available in workerd. Use 'pbkdf2' instead.`
      );
    }
    const cost = options?.cost ?? DEFAULT_PBKDF2_COST;
    const iterations = 2 ** cost * 100;
    const salt = crypto.getRandomValues(
      new Uint8Array(DEFAULT_PBKDF2_SALT_BYTES)
    );
    const hashHex = await pbkdf2Hash(password2, iterations, salt);
    return `$workerd$pbkdf2$sha256$${iterations}$${bytesToHex(salt)}$${hashHex}`;
  },
  async verify(password2, hash2) {
    if (hash2.startsWith("$workerd$pbkdf2$sha256$")) {
      const parts = hash2.split("$");
      if (parts.length !== 7) throw new Error("Invalid workerd hash format");
      const iterationsRaw = parts[4];
      const saltHex = parts[5];
      const expectedHashHex = parts[6];
      if (iterationsRaw === void 0 || saltHex === void 0 || expectedHashHex === void 0) {
        throw new Error("Invalid workerd hash format");
      }
      if (!/^\d+$/.test(iterationsRaw))
        throw new Error("Invalid workerd hash format");
      const iterations = parseInt(iterationsRaw, 10);
      if (!Number.isSafeInteger(iterations) || iterations <= 0 || iterations > 4294967295) {
        throw new Error("Invalid workerd hash format");
      }
      if (!/^[0-9a-f]+$/i.test(saltHex) || saltHex.length % 2 !== 0) {
        throw new Error("Invalid workerd hash format");
      }
      if (!/^[0-9a-f]+$/i.test(expectedHashHex) || expectedHashHex.length === 0) {
        throw new Error("Invalid workerd hash format");
      }
      const salt = hexToBytes(saltHex);
      const computedHashHex = await pbkdf2Hash(password2, iterations, salt);
      return constantTimeEqual(computedHashHex, expectedHashHex);
    }
    if (hash2.startsWith("$workerd$")) {
      return verifyLegacyWorkerdHash(password2, hash2);
    }
    throw new Error("Unknown hash format. Expected workerd PBKDF2 hash.");
  }
};
async function verifyLegacyWorkerdHash(password2, hash2) {
  const parts = hash2.split("$");
  if (parts.length !== 6) {
    throw new Error(
      "Invalid legacy workerd hash format. Expected: $workerd$pbkdf2$<cost>$<saltHex>$<hashHex>"
    );
  }
  const algorithm = parts[2];
  if (algorithm !== "pbkdf2") {
    throw new Error(
      `Unknown legacy hash algorithm: ${algorithm}. Only 'pbkdf2' is supported.`
    );
  }
  const costStr = parts[3];
  const saltHex = parts[4];
  const expectedHashHex = parts[5];
  if (costStr === void 0 || saltHex === void 0 || expectedHashHex === void 0) {
    throw new Error("Invalid legacy workerd hash format: missing components");
  }
  if (!/^\d+$/.test(costStr)) {
    throw new Error("Invalid legacy workerd hash format: cost must be numeric");
  }
  const cost = parseInt(costStr, 10);
  if (!Number.isSafeInteger(cost) || cost < 0 || cost > 31) {
    throw new Error(
      "Invalid legacy workerd hash format: cost must be between 0 and 31"
    );
  }
  if (!/^[0-9a-f]+$/i.test(saltHex) || saltHex.length % 2 !== 0) {
    throw new Error(
      "Invalid legacy workerd hash format: salt must be valid hex string"
    );
  }
  if (!/^[0-9a-f]+$/i.test(expectedHashHex) || expectedHashHex.length === 0) {
    throw new Error(
      "Invalid legacy workerd hash format: hash must be valid non-empty hex string"
    );
  }
  const iterations = 2 ** cost * 100;
  const salt = hexToBytes(saltHex);
  const computedHashHex = await pbkdf2Hash(password2, iterations, salt);
  return constantTimeEqual(computedHashHex, expectedHashHex);
}
async function readableStreamToArray(stream) {
  const reader = stream.getReader();
  const chunks = [];
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
  }
  return chunks;
}
async function readableStreamToText(stream) {
  const chunks = await readableStreamToArray(stream);
  const decoder = new TextDecoder();
  return chunks.map((chunk) => decoder.decode(chunk, { stream: true })).join("");
}
async function readableStreamToArrayBuffer(stream) {
  const chunks = await readableStreamToArray(stream);
  const totalLength = chunks.reduce((sum, chunk) => sum + chunk.byteLength, 0);
  const result = new Uint8Array(totalLength);
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return result.buffer;
}
async function readableStreamToBlob(stream, type) {
  const buffer = await readableStreamToArrayBuffer(stream);
  return type !== void 0 ? new Blob([buffer], { type }) : new Blob([buffer]);
}
async function readableStreamToJSON(stream) {
  return JSON.parse(await readableStreamToText(stream));
}
var ArrayBufferSink = class {
  chunks = [];
  write(data) {
    const bytes = typeof data === "string" ? new TextEncoder().encode(data) : data instanceof ArrayBuffer ? new Uint8Array(data) : data;
    this.chunks.push(bytes);
  }
  end() {
    const totalLength = this.chunks.reduce((sum, c) => sum + c.byteLength, 0);
    const result = new Uint8Array(totalLength);
    let offset = 0;
    for (const chunk of this.chunks) {
      result.set(chunk, offset);
      offset += chunk.byteLength;
    }
    this.chunks = [];
    return result.buffer;
  }
  flush() {
  }
  // no-op, data accumulated until end()
  start() {
    this.chunks = [];
  }
};
var main = (() => {
  const proc = getProcess();
  return proc?.argv?.[1] !== void 0;
})();
function randomUUIDv7() {
  const timestamp = Date.now();
  const timestampHex = timestamp.toString(16).padStart(12, "0");
  const random = crypto.getRandomValues(new Uint8Array(10));
  const randomHex = Array.from(random).map((b) => b.toString(16).padStart(2, "0")).join("");
  return [
    timestampHex.slice(0, 8),
    timestampHex.slice(8, 12),
    `7${randomHex.slice(0, 3)}`,
    (parseInt(randomHex.slice(3, 5), 16) & 63 | 128).toString(16).padStart(2, "0") + randomHex.slice(5, 7),
    randomHex.slice(7, 19)
  ].join("-");
}
var resolvedPromises = /* @__PURE__ */ new WeakMap();
function peek(promise) {
  const cached = resolvedPromises.get(promise);
  if (cached) {
    if (cached.resolved) {
      if (cached.error !== void 0) throw cached.error;
      return cached.value;
    }
    return promise;
  }
  const tracker = {
    resolved: false,
    value: void 0,
    error: void 0
  };
  resolvedPromises.set(promise, tracker);
  promise.then((value) => {
    tracker.resolved = true;
    tracker.value = value;
  }).catch((error) => {
    tracker.resolved = true;
    tracker.error = error;
  });
  return promise;
}
var gcWarningShown = false;
var shrinkWarningShown = false;
function gc() {
  if (!gcWarningShown) {
    console.warn(
      "[Bun.gc] No-op in workerd: V8 isolate garbage collection is managed by the runtime."
    );
    gcWarningShown = true;
  }
}
function shrink() {
  if (!shrinkWarningShown) {
    console.warn(
      "[Bun.shrink] No-op in workerd: Memory management is handled by the runtime."
    );
    shrinkWarningShown = true;
  }
}
function generateHeapSnapshot() {
  throw new Error("generateHeapSnapshot is not available in workerd");
}
function openInEditor(_path, _options) {
  throw new Error("openInEditor is not available in workerd");
}
function fileURLToPath(url) {
  const urlObj = typeof url === "string" ? new URL(url) : url;
  if (urlObj.protocol !== "file:")
    throw new Error("URL must use file: protocol");
  return urlObj.pathname;
}
function pathToFileURL(path) {
  return new URL(`file://${path.startsWith("/") ? "" : "/"}${path}`);
}
var bun_default = {
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
  dns: dns_exports,
  main,
  randomUUIDv7,
  peek,
  gc,
  shrink,
  generateHeapSnapshot,
  openInEditor,
  fileURLToPath,
  pathToFileURL
};
export {
  ArrayBufferSink,
  deepEquals,
  bun_default as default,
  dns_exports as dns,
  env,
  escapeHTML,
  file,
  fileURLToPath,
  gc,
  generateHeapSnapshot,
  getServeHandler,
  hash,
  inspect,
  main,
  nanoseconds,
  openInEditor,
  password,
  pathToFileURL,
  peek,
  randomUUIDv7,
  readableStreamToArray,
  readableStreamToArrayBuffer,
  readableStreamToBlob,
  readableStreamToJSON,
  readableStreamToText,
  revision,
  serve,
  shrink,
  sleep,
  sleepSync,
  stringWidth,
  version,
  write
};
