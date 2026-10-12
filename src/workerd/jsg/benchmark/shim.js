// Minimal web-API shim so wrangler-style bundles (hono etc.) run in d8.

class Headers {
  #map = new Map();
  constructor(init) {
    if (init instanceof Headers) {
      for (const [k, v] of init) this.#map.set(k, v);
    } else if (Array.isArray(init)) {
      for (const [k, v] of init) this.append(k, v);
    } else if (init) {
      for (const k of Object.keys(init)) this.set(k, init[k]);
    }
  }
  get(name) { return this.#map.get(String(name).toLowerCase()) ?? null; }
  set(name, value) { this.#map.set(String(name).toLowerCase(), String(value)); }
  append(name, value) {
    const key = String(name).toLowerCase();
    const prev = this.#map.get(key);
    this.#map.set(key, prev === undefined ? String(value) : prev + ", " + String(value));
  }
  has(name) { return this.#map.has(String(name).toLowerCase()); }
  delete(name) { this.#map.delete(String(name).toLowerCase()); }
  forEach(cb, thisArg) { for (const [k, v] of this.#map) cb.call(thisArg, v, k, this); }
  *entries() { yield* this.#map.entries(); }
  *keys() { yield* this.#map.keys(); }
  *values() { yield* this.#map.values(); }
  [Symbol.iterator]() { return this.#map.entries(); }
  getSetCookie() { return []; }
}

class URLSearchParams {
  #params = [];
  constructor(init = "") {
    if (typeof init === "string") {
      const s = init.startsWith("?") ? init.slice(1) : init;
      if (s.length > 0) {
        for (const pair of s.split("&")) {
          const i = pair.indexOf("=");
          if (i < 0) this.#params.push([decodeURIComponent(pair), ""]);
          else this.#params.push([decodeURIComponent(pair.slice(0, i)), decodeURIComponent(pair.slice(i + 1).replace(/\+/g, " "))]);
        }
      }
    } else if (init) {
      for (const k of Object.keys(init)) this.#params.push([k, String(init[k])]);
    }
  }
  get(name) { const p = this.#params.find((e) => e[0] === name); return p ? p[1] : null; }
  getAll(name) { return this.#params.filter((e) => e[0] === name).map((e) => e[1]); }
  has(name) { return this.#params.some((e) => e[0] === name); }
  append(k, v) { this.#params.push([k, String(v)]); }
  set(k, v) { this.#params = this.#params.filter((e) => e[0] !== k); this.#params.push([k, String(v)]); }
  *entries() { yield* this.#params; }
  *keys() { for (const e of this.#params) yield e[0]; }
  [Symbol.iterator]() { return this.#params[Symbol.iterator](); }
  forEach(cb) { for (const [k, v] of this.#params) cb(v, k, this); }
  toString() { return this.#params.map(([k, v]) => encodeURIComponent(k) + "=" + encodeURIComponent(v)).join("&"); }
}

class URL {
  constructor(href, base) {
    if (base !== undefined && !/^[a-z]+:/i.test(href)) {
      const b = new URL(base);
      href = b.origin + (href.startsWith("/") ? href : b.pathname.replace(/[^/]*$/, "") + href);
    }
    const m = /^([a-z][a-z0-9+.-]*):\/\/([^/?#:]*)(?::(\d+))?(\/[^?#]*)?(\?[^#]*)?(#.*)?$/i.exec(href);
    if (!m) throw new TypeError("Invalid URL: " + href);
    this.protocol = m[1].toLowerCase() + ":";
    this.hostname = m[2];
    this.port = m[3] ?? "";
    this.host = this.port ? this.hostname + ":" + this.port : this.hostname;
    this.pathname = m[4] || "/";
    this.search = m[5] && m[5] !== "?" ? m[5] : "";
    this.hash = m[6] || "";
    this.origin = this.protocol + "//" + this.host;
    this.searchParams = new URLSearchParams(this.search);
  }
  get href() { return this.origin + this.pathname + this.search + this.hash; }
  toString() { return this.href; }
}

class Body {
  #body;
  bodyUsed = false;
  constructor(body) { this.#body = body ?? null; }
  get body() { return this.#body; }
  async text() { this.bodyUsed = true; return this.#body === null ? "" : String(this.#body); }
  async json() { return JSON.parse(await this.text()); }
  async arrayBuffer() { return new TextEncoder().encode(await this.text()).buffer; }
  async formData() { throw new Error("formData not supported in shim"); }
  async blob() { throw new Error("blob not supported in shim"); }
}

class Request extends Body {
  constructor(input, init = {}) {
    super(init.body);
    if (input instanceof Request) {
      this.url = input.url;
      this.method = init.method ?? input.method;
      this.headers = new Headers(init.headers ?? input.headers);
    } else {
      this.url = String(input);
      this.method = (init.method ?? "GET").toUpperCase();
      this.headers = new Headers(init.headers);
    }
    this.signal = init.signal ?? null;
    this.cf = init.cf;
  }
  clone() { return new Request(this, { body: this.body }); }
}

const STATUS_TEXT = new Map([[200, "OK"], [201, "Created"], [204, "No Content"], [301, "Moved Permanently"], [302, "Found"], [400, "Bad Request"], [401, "Unauthorized"], [404, "Not Found"], [500, "Internal Server Error"]]);

class Response extends Body {
  constructor(body, init = {}) {
    super(body);
    this.status = init.status ?? 200;
    this.statusText = init.statusText ?? STATUS_TEXT.get(this.status) ?? "";
    this.headers = new Headers(init.headers);
    this.ok = this.status >= 200 && this.status < 300;
  }
  static json(data, init = {}) {
    const headers = new Headers(init.headers);
    if (!headers.has("content-type")) headers.set("content-type", "application/json");
    return new Response(JSON.stringify(data), { ...init, headers });
  }
  static redirect(url, status = 302) { return new Response(null, { status, headers: { location: String(url) } }); }
  clone() { return new Response(this.body, this); }
}

class TextEncoder {
  encode(s) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
      let c = s.charCodeAt(i);
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
  }
}

class TextDecoder {
  decode(u8) { let s = ""; for (const b of u8) s += String.fromCharCode(b); return s; }
}

const crypto = {
  randomUUID() { return "00000000-0000-4000-8000-000000000000"; },
  getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = (i * 7919) & 0xff; return a; },
  subtle: {},
};

Object.assign(globalThis, {
  Headers, URL, URLSearchParams, Request, Response, TextEncoder, TextDecoder, crypto,
  console: globalThis.console ?? { log: print, error: print, warn: print, info: print, debug: print },
  self: globalThis,
  setTimeout(fn) { Promise.resolve().then(fn); return 0; },
  clearTimeout() {},
  queueMicrotask: (fn) => Promise.resolve().then(fn),
  performance: globalThis.performance ?? { now: () => 0 },
});
