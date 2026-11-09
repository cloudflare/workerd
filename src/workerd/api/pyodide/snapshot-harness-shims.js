// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Runs as a classic script in the Pyodide snapshot harness context before any module is
// evaluated. The context is a bare V8 context: this installs the small set of Web platform
// globals that the Pyodide runtime bundle needs to load, and routes every WebAssembly
// instantiation through the harness so that instances are recorded for the snapshot.
//
// `harness` is the native object installed by the C++ side; see snapshot-harness.c++.
(function (harness) {
  'use strict';
  const g = globalThis;

  // ---------------------------------------------------------------------------------------------
  // Globals a worker context does not have. WeakRef and FinalizationRegistry are gated by the
  // enable_weak_ref compatibility flag (Pyodide finalizes PyProxies through FinalizationRegistry
  // if it exists when it loads, so the flag is part of the snapshot key); Temporal is never
  // exposed to Python.

  const compatibilityFlags = JSON.parse(harness.compatibilityFlags());
  if (!compatibilityFlags.enable_weak_ref) {
    delete g.WeakRef;
    delete g.FinalizationRegistry;
  }
  delete g.Temporal;
  delete Date.prototype.toTemporalInstant;

  // ---------------------------------------------------------------------------------------------
  // Host services: the clock, console, timers and randomness. While the snapshot is being built
  // they are the harness's own; an embedder that restores the snapshot replaces them with its
  // host's (harness.setHost()), so that Python, whose Emscripten runtime reaches these through the
  // globals defined below, observes the host's clock and writes to the host's console. The globals
  // forward on every call, so JavaScript that captured them before the snapshot follows along.

  function describe(v) {
    if (typeof v === 'string') return v;
    if (v instanceof Error) return v.stack || String(v);
    try {
      return JSON.stringify(v) ?? String(v);
    } catch {
      return String(v);
    }
  }
  const fmt = (args) => args.map(describe).join(' ');

  const timers = new Map();
  let nextTimerId = 1;
  function schedule(fn, delay, args, repeat) {
    const id = nextTimerId++;
    delay = Math.max(0, Number(delay) || 0);
    timers.set(id, { fn, args, due: harness.now() + delay, delay, repeat });
    return id;
  }
  // Fires every timer that is due. Returns { ran, next }: the number of timers fired and the time
  // in ms until the next pending timer is due (-1 if none are pending). The harness drives this
  // from C++ between microtask checkpoints while building the snapshot.
  harness.runTimers = function runTimers() {
    const now = harness.now();
    let ran = 0;
    for (const [id, t] of [...timers]) {
      if (t.due > now) continue;
      if (t.repeat) {
        t.due = now + t.delay;
      } else {
        timers.delete(id);
      }
      ran++;
      if (typeof t.fn === 'function') t.fn(...t.args);
    }
    let next = -1;
    for (const t of timers.values()) {
      const wait = Math.max(0, t.due - harness.now());
      next = next < 0 ? wait : Math.min(next, wait);
    }
    return { ran, next };
  };

  const harnessHost = {
    console: {
      debug: (...a) => harness.print(0, fmt(a)),
      log: (...a) => harness.print(1, fmt(a)),
      info: (...a) => harness.print(1, fmt(a)),
      warn: (...a) => harness.print(2, fmt(a)),
      error: (...a) => harness.print(3, fmt(a)),
    },
    performance: { now: () => harness.now() },
    setTimeout: (fn, delay, ...args) => schedule(fn, delay, args, false),
    setInterval: (fn, delay, ...args) => schedule(fn, delay, args, true),
    clearTimeout: (id) => timers.delete(id),
    clearInterval: (id) => timers.delete(id),
    crypto: {
      getRandomValues(array) {
        harness.randomFill(array);
        return array;
      },
    },
  };
  let host = harnessHost;
  harness.setHost = (h) => {
    host = h;
  };
  // Replaces the harness global with the little the shim modules still need after the snapshot
  // has been restored, so that the natives that read the Pyodide bundle and compile WebAssembly
  // are no longer reachable from the context. The shims themselves keep `harness` in this closure.
  harness.seal = () => {
    const sealed = Object.freeze({
      runTimers: harness.runTimers,
      pyodideVersion: () => harness.pyodideVersion(),
      compatibilityFlags: () => harness.compatibilityFlags(),
    });
    Object.defineProperty(g, '__pyodideSnapshotHarness', {
      value: sealed,
      writable: false,
      configurable: false,
    });
  };

  const consoleMethod =
    (name) =>
    (...a) =>
      host.console[name](...a);
  g.console = {
    debug: consoleMethod('debug'),
    log: consoleMethod('log'),
    info: consoleMethod('info'),
    warn: consoleMethod('warn'),
    error: consoleMethod('error'),
    trace: consoleMethod('warn'),
    assert(cond, ...a) {
      if (!cond) host.console.error('Assertion failed: ' + fmt(a));
    },
    table: (x) => host.console.log(describe(x)),
    time() {},
    timeEnd() {},
    group() {},
    groupEnd() {},
  };
  g.setTimeout = (...a) => host.setTimeout(...a);
  g.setInterval = (...a) => host.setInterval(...a);
  g.clearTimeout = (id) => host.clearTimeout(id);
  g.clearInterval = (id) => host.clearInterval(id);
  g.performance = { now: () => host.performance.now(), timeOrigin: 0 };
  g.crypto = {
    getRandomValues: (array) => host.crypto.getRandomValues(array),
    randomUUID() {
      const b = new Uint8Array(16);
      host.crypto.getRandomValues(b);
      b[6] = (b[6] & 0x0f) | 0x40;
      b[8] = (b[8] & 0x3f) | 0x80;
      const h = [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
      return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
    },
  };

  g.queueMicrotask = (fn) => {
    Promise.resolve().then(fn);
  };
  g.addEventListener = () => {};
  g.removeEventListener = () => {};
  g.dispatchEvent = () => true;
  g.navigator = { userAgent: 'Cloudflare-Workers', hardwareConcurrency: 1 };
  g.self = g;

  // ---------------------------------------------------------------------------------------------
  // base64

  const B64 =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  g.btoa = (s) => {
    s = String(s);
    let out = '';
    for (let i = 0; i < s.length; i += 3) {
      const c0 = s.charCodeAt(i);
      const c1 = s.charCodeAt(i + 1);
      const c2 = s.charCodeAt(i + 2);
      if (c0 > 255 || c1 > 255 || c2 > 255)
        throw new Error('btoa: invalid character');
      const n = (c0 << 16) | ((c1 || 0) << 8) | (c2 || 0);
      out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63];
      out += i + 1 < s.length ? B64[(n >> 6) & 63] : '=';
      out += i + 2 < s.length ? B64[n & 63] : '=';
    }
    return out;
  };
  g.atob = (s) => {
    s = String(s).replace(/[\s=]/g, '');
    let out = '';
    let bits = 0;
    let acc = 0;
    for (const ch of s) {
      const v = B64.indexOf(ch);
      if (v < 0) throw new Error('atob: invalid character');
      acc = (acc << 6) | v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out += String.fromCharCode((acc >> bits) & 255);
      }
    }
    return out;
  };

  // ---------------------------------------------------------------------------------------------
  // TextEncoder / TextDecoder (utf-8, utf-16le, latin1)

  class TextEncoder {
    get encoding() {
      return 'utf-8';
    }
    encode(s = '') {
      s = String(s);
      const out = new Uint8Array(s.length * 3);
      const { written } = this.encodeInto(s, out);
      return out.subarray(0, written);
    }
    encodeInto(s, out) {
      s = String(s);
      let read = 0;
      let written = 0;
      const n = out.length;
      while (read < s.length) {
        let cp = s.codePointAt(read);
        const units = cp > 0xffff ? 2 : 1;
        if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd; // lone surrogate
        const need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (written + need > n) break;
        if (need === 1) {
          out[written++] = cp;
        } else if (need === 2) {
          out[written++] = 0xc0 | (cp >> 6);
          out[written++] = 0x80 | (cp & 63);
        } else if (need === 3) {
          out[written++] = 0xe0 | (cp >> 12);
          out[written++] = 0x80 | ((cp >> 6) & 63);
          out[written++] = 0x80 | (cp & 63);
        } else {
          out[written++] = 0xf0 | (cp >> 18);
          out[written++] = 0x80 | ((cp >> 12) & 63);
          out[written++] = 0x80 | ((cp >> 6) & 63);
          out[written++] = 0x80 | (cp & 63);
        }
        read += units;
      }
      return { read, written };
    }
  }

  function normalizeEncoding(label) {
    const l = String(label ?? 'utf-8')
      .trim()
      .toLowerCase();
    if (['utf-8', 'utf8', 'unicode-1-1-utf-8'].includes(l)) return 'utf-8';
    if (['utf-16le', 'utf-16', 'ucs-2', 'utf16le'].includes(l))
      return 'utf-16le';
    if (
      [
        'latin1',
        'iso-8859-1',
        'ascii',
        'us-ascii',
        'windows-1252',
        'l1',
      ].includes(l)
    ) {
      return 'latin1';
    }
    throw new RangeError(`TextDecoder: unsupported encoding "${label}"`);
  }

  function toBytes(input) {
    if (input === undefined) return new Uint8Array(0);
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (ArrayBuffer.isView(input)) {
      return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    }
    throw new TypeError(
      'TextDecoder.decode: expected an ArrayBuffer or ArrayBufferView'
    );
  }

  function unitsToString(units) {
    let out = '';
    for (let i = 0; i < units.length; i += 8192) {
      out += String.fromCharCode.apply(
        null,
        units.subarray(i, Math.min(i + 8192, units.length))
      );
    }
    return out;
  }

  class TextDecoder {
    #encoding;
    #fatal;
    #ignoreBOM;
    constructor(label = 'utf-8', options = {}) {
      this.#encoding = normalizeEncoding(label);
      this.#fatal = !!options.fatal;
      this.#ignoreBOM = !!options.ignoreBOM;
    }
    get encoding() {
      return this.#encoding;
    }
    get fatal() {
      return this.#fatal;
    }
    get ignoreBOM() {
      return this.#ignoreBOM;
    }
    decode(input, _options) {
      const bytes = toBytes(input);
      if (this.#encoding === 'latin1') return unitsToString(bytes);
      if (this.#encoding === 'utf-16le') {
        const n = bytes.length >> 1;
        const units = new Uint16Array(n);
        for (let i = 0; i < n; i++)
          units[i] = bytes[2 * i] | (bytes[2 * i + 1] << 8);
        let start = 0;
        if (!this.#ignoreBOM && n > 0 && units[0] === 0xfeff) start = 1;
        return unitsToString(units.subarray(start));
      }
      return this.#decodeUtf8(bytes);
    }
    #decodeUtf8(bytes) {
      const n = bytes.length;
      const units = new Uint16Array(n); // upper bound: one unit per byte, plus surrogates
      let out = 0;
      let i = 0;
      if (
        !this.#ignoreBOM &&
        n >= 3 &&
        bytes[0] === 0xef &&
        bytes[1] === 0xbb &&
        bytes[2] === 0xbf
      ) {
        i = 3;
      }
      const bad = () => {
        if (this.#fatal)
          throw new TypeError('TextDecoder.decode: invalid UTF-8');
        units[out++] = 0xfffd;
      };
      while (i < n) {
        const b0 = bytes[i++];
        if (b0 < 0x80) {
          units[out++] = b0;
          continue;
        }
        let need;
        let cp;
        let min;
        if (b0 >= 0xc2 && b0 <= 0xdf) {
          need = 1;
          cp = b0 & 0x1f;
          min = 0x80;
        } else if (b0 >= 0xe0 && b0 <= 0xef) {
          need = 2;
          cp = b0 & 0x0f;
          min = 0x800;
        } else if (b0 >= 0xf0 && b0 <= 0xf4) {
          need = 3;
          cp = b0 & 0x07;
          min = 0x10000;
        } else {
          bad();
          continue;
        }
        if (i + need > n) {
          bad();
          i = n;
          break;
        }
        let ok = true;
        for (let k = 0; k < need; k++) {
          const b = bytes[i + k];
          if ((b & 0xc0) !== 0x80) {
            ok = false;
            break;
          }
          cp = (cp << 6) | (b & 0x3f);
        }
        if (
          !ok ||
          cp < min ||
          cp > 0x10ffff ||
          (cp >= 0xd800 && cp <= 0xdfff)
        ) {
          bad();
          continue;
        }
        i += need;
        if (cp > 0xffff) {
          cp -= 0x10000;
          units[out++] = 0xd800 | (cp >> 10);
          units[out++] = 0xdc00 | (cp & 0x3ff);
        } else {
          units[out++] = cp;
        }
      }
      return unitsToString(units.subarray(0, out));
    }
  }
  g.TextEncoder = TextEncoder;
  g.TextDecoder = TextDecoder;

  // ---------------------------------------------------------------------------------------------
  // WebAssembly interception. Every compile registers the module (so the restore side can find
  // its bytes again) and every instantiation goes through the recorder, which hands back stubs.

  const RealModule = WebAssembly.Module;
  const RealInstance = WebAssembly.Instance;

  function Module(bytes) {
    return harness.compileWasm(bytes);
  }
  Module.prototype = RealModule.prototype;
  // Module stubs are plain objects, and after the snapshot is restored a module may also be a
  // real one from another context, so `instanceof` (which Emscripten's shared library loader
  // uses to tell a compiled module from its bytes) asks the native.
  Object.defineProperty(Module, Symbol.hasInstance, {
    value: (value) => harness.isModule(value),
  });
  // The statics receive module stubs; map them to the real module for the duration of the call.
  Module.exports = (m) => RealModule.exports(harness.realModule(m));
  Module.imports = (m) => RealModule.imports(harness.realModule(m));
  Module.customSections = (m, name) =>
    RealModule.customSections(harness.realModule(m), name);
  WebAssembly.Module = Module;

  // Pyodide's error sentinel module (pool/sentinel.ts) returns a WebAssembly GC struct that
  // Pyodide then keeps in JS as `Module.error`. GC objects cannot be part of a snapshot (their
  // types live in a process-wide table), and the main module only ever sees these two functions
  // as imports, so a plain JS object with the same identity semantics stands in for it.
  const jsvErrorSentinel = Object.freeze({
    __proto__: null,
    pyodideErrorSentinel: true,
  });
  function isJsvErrorModule(module) {
    const exports = Module.exports(module);
    return (
      exports.length === 2 &&
      exports.some(
        (e) => e.name === 'Jsv_GetError_import' && e.kind === 'function'
      ) &&
      exports.some((e) => e.name === 'JsvError_Check' && e.kind === 'function')
    );
  }
  // `module` is a harness module stub, or, once the snapshot has been restored, possibly a real
  // WebAssembly.Module from another context (a shared library compiled by the adopting worker);
  // `instanceof RealModule` would reject the latter, so the native decides.
  function instantiateAny(module, imports) {
    if (typeof module !== 'object' || module === null) {
      throw new TypeError(
        'WebAssembly.Instance(): Argument 0 must be a WebAssembly.Module'
      );
    }
    if (isJsvErrorModule(module)) {
      return {
        exports: Object.freeze({
          Jsv_GetError_import: () => jsvErrorSentinel,
          JsvError_Check: (value) => (value === jsvErrorSentinel ? 1 : 0),
        }),
      };
    }
    return harness.instantiate(module, imports);
  }

  function Instance(module, imports = {}) {
    return instantiateAny(module, imports);
  }
  Instance.prototype = RealInstance.prototype;
  WebAssembly.Instance = Instance;

  // Standalone memories cannot be part of the snapshot: V8 links a wasm memory's buffer back to
  // the memory object, so a view kept in JS would drag the real memory into the serialized heap.
  // The only one Pyodide creates is the one-page shared memory Emscripten's Atomics-based sleep
  // waits on; a plain (never shared: V8 cannot carry a SharedArrayBuffer through a snapshot)
  // buffer stands in for it. Atomics.wait() on it throws, which Emscripten's syncSleep() treats
  // as "cannot sleep" exactly as it does in workerd, where Atomics.wait() is disallowed.
  const RealMemory = WebAssembly.Memory;
  function Memory(descriptor) {
    const buffer = new ArrayBuffer((Number(descriptor?.initial) || 0) * 65536);
    return {
      get buffer() {
        return buffer;
      },
      grow() {
        throw new RangeError(
          'WebAssembly.Memory.grow() is not supported in the snapshot harness'
        );
      },
    };
  }
  Memory.prototype = RealMemory.prototype;
  WebAssembly.Memory = Memory;

  // WebAssembly.promising() only accepts real exported functions, and its result wraps the live
  // instance, so neither the argument (a trampoline) nor the result can be used directly. The
  // wrapper resolves and caches the real promising function on first call; the cache is dropped
  // at freeze time so nothing referring to the live instance reaches the snapshot.
  const realPromising = WebAssembly.promising;
  let promisingCache = new WeakMap();
  harness.dropPromisingCache = () => {
    promisingCache = new WeakMap();
  };
  if (typeof realPromising === 'function') {
    WebAssembly.promising = function promising(fn) {
      if (typeof fn !== 'function') {
        throw new TypeError(
          'WebAssembly.promising(): Argument 0 must be a function'
        );
      }
      const wrapper = function (...args) {
        let real = promisingCache.get(wrapper);
        if (real === undefined) {
          real = realPromising(harness.realFunction(fn));
          promisingCache.set(wrapper, real);
        }
        return real.apply(this, args);
      };
      return wrapper;
    };
  }

  WebAssembly.compile = (bytes) =>
    Promise.resolve().then(() => harness.compileWasm(bytes));
  WebAssembly.instantiate = (source, imports = {}) =>
    Promise.resolve().then(() => {
      if (source instanceof RealModule) return instantiateAny(source, imports);
      const module = harness.compileWasm(source);
      return { module, instance: instantiateAny(module, imports) };
    });
})(globalThis.__pyodideSnapshotHarness);
