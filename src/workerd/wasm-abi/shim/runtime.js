// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The shared runtime of the shim, the reference implementation of the Workers Wasm ABI on the
// JavaScript Workers API. `../conventions.md` is the specification; the shim is its executable
// form, and any native implementation must be observably equivalent to it.
//
// The runtime holds what every module shares: the per-request handle table, guest memory
// access, the marshalling of parameters and results, suspension, pre-initialization and
// `serve()`. It implements the imports of each Wasm import module from the descriptors in that
// module's generated shim module (`cf-<name>-v<n>.js`): lowered imports by calling their JSG
// member, conventions-defined imports with the implementations in DEFINED below.

// A fault caused by the guest (bad handle, pointer out of bounds, blocking in a callback,
// calling an import during init). Faults are never converted into error handles; they unwind
// out of the guest and fail the request, like a trap.
class GuestFault extends Error {
  constructor(message, options) {
    super(message, options);
    this.name = 'GuestFault';
  }
}

const utf8Decoder = new TextDecoder('utf-8', { fatal: false, ignoreBOM: true });
const utf8Encoder = new TextEncoder();

const VALUE_KIND = {
  undefined: 0,
  null: 1,
  boolean: 2,
  number: 3,
  bigint: 4,
  string: 5,
  symbol: 6,
  function: 7,
  object: 8,
  array: 9,
  bytes: 10,
};

function valueKind(v) {
  if (v === null) return VALUE_KIND.null;
  if (Array.isArray(v)) return VALUE_KIND.array;
  if (v instanceof ArrayBuffer || ArrayBuffer.isView(v))
    return VALUE_KIND.bytes;
  return VALUE_KIND[typeof v];
}

function isAbsent(v) {
  return v === undefined || v === null;
}

function globalCtor(name) {
  if (name === null || name === undefined) return undefined;
  const c = globalThis[name];
  return typeof c === 'function' ? c : undefined;
}

function toBytes(v) {
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  if (ArrayBuffer.isView(v))
    return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
  throw new TypeError('expected an ArrayBuffer or ArrayBuffer view');
}

// Per-request state: the instance, its memory, and the handle table, which all modules share.
class RequestState {
  constructor(options) {
    this.table = [undefined]; // Index 0 is the null handle.
    this.live = [false];
    this.free = []; // LIFO free list.
    this.liveCount = 0;
    this.instance = null;
    this.inInit = false;
    this.onRespond = null;
    this.trace = options.trace ?? null;
  }

  // ---- Handle table. ----

  add(v) {
    let h;
    if (this.free.length > 0) {
      h = this.free.pop();
    } else {
      h = this.table.length;
      this.table.push(undefined);
      this.live.push(false);
    }
    this.table[h] = v;
    this.live[h] = true;
    this.liveCount++;
    return h;
  }

  get(h) {
    if (
      !Number.isInteger(h) ||
      h <= 0 ||
      h >= this.table.length ||
      !this.live[h]
    ) {
      throw new GuestFault(`invalid handle ${h}`);
    }
    return this.table[h];
  }

  drop(h) {
    this.get(h);
    this.table[h] = undefined;
    this.live[h] = false;
    this.liveCount--;
    this.free.push(h);
  }

  // ---- Guest memory. ----

  bytes() {
    return new Uint8Array(this.instance.exports.memory.buffer);
  }

  view() {
    return new DataView(this.instance.exports.memory.buffer);
  }

  checkRange(ptr, len) {
    const size = this.instance.exports.memory.buffer.byteLength;
    if (ptr < 0 || len < 0 || ptr + len > size) {
      throw new GuestFault(
        `guest range [${ptr}, ${ptr + len}) outside memory of ${size} bytes`
      );
    }
  }

  readBytes(ptr, len) {
    ptr >>>= 0;
    len >>>= 0;
    this.checkRange(ptr, len);
    return this.bytes().slice(ptr, ptr + len);
  }

  readString(ptr, len) {
    ptr >>>= 0;
    len >>>= 0;
    this.checkRange(ptr, len);
    return utf8Decoder.decode(this.bytes().subarray(ptr, ptr + len));
  }

  // Writes a byte result into a string/bytes slot {buf_ptr, buf_cap, len, overflow}.
  writeBytesSlot(slotPtr, data) {
    this.checkRange(slotPtr, 16);
    const view = this.view();
    const bufPtr = view.getUint32(slotPtr, true);
    const bufCap = view.getUint32(slotPtr + 4, true);
    let overflow = 0;
    if (data.length <= bufCap) {
      this.checkRange(bufPtr, data.length);
      this.bytes().set(data, bufPtr);
    } else {
      overflow = this.add(data.slice());
    }
    view.setUint32(slotPtr + 8, data.length, true);
    view.setUint32(slotPtr + 12, overflow, true);
  }

  // ---- Callbacks. ----

  callback(token, args) {
    const argsHandle = this.add(args);
    let r;
    try {
      r = this.instance.exports.cf_callback(token, argsHandle);
    } catch (e) {
      throw e instanceof GuestFault
        ? e
        : new GuestFault('guest callback trapped', { cause: e });
    } finally {
      if (this.live[argsHandle]) this.drop(argsHandle);
    }
    if (r === 0) return undefined;
    if (r > 0) {
      const v = this.get(r);
      this.drop(r);
      return v;
    }
    const err = this.get(-r);
    this.drop(-r);
    throw err;
  }
}

// ---- Parameter decoding: consumes Wasm arguments for one L-type. ----

function decodeParam(st, lt, w, i) {
  switch (lt.k) {
    case 'bool':
      return [w[i] !== 0, i + 1];
    case 'i32':
      return [lt.unsigned ? w[i] >>> 0 : w[i], i + 1];
    case 'i64':
      return [
        lt.bigint ? BigInt.asIntN(64, w[i]) : Number(BigInt.asIntN(64, w[i])),
        i + 1,
      ];
    case 'f64':
      return [lt.date ? new Date(w[i]) : w[i], i + 1];
    case 'string':
      return [st.readString(w[i], w[i + 1]), i + 2];
    case 'bytes': {
      const b = st.readBytes(w[i], w[i + 1]);
      return [lt.js === 'ArrayBuffer' ? b.buffer : b, i + 2];
    }
    case 'handle':
    case 'varargs':
      return [st.get(w[i]), i + 1];
    case 'callback': {
      const token = w[i];
      return [(...args) => st.callback(token, args), i + 1];
    }
    case 'maybe': {
      const absent = lt.absent === 'null' ? null : undefined;
      if (lt.inner.k === 'handle' || lt.inner.k === 'callback') {
        if (w[i] === 0) return [absent, i + 1];
        return decodeParam(st, lt.inner, w, i);
      }
      const width = wasmTypes(lt.inner).length;
      if (w[i] === 0) return [absent, i + 1 + width];
      return decodeParam(st, lt.inner, w, i + 1);
    }
    case 'oneof': {
      const tag = w[i];
      const width = lt.payload.length;
      if (tag < 0 || tag >= lt.variants.length)
        throw new GuestFault(`invalid union tag ${tag}`);
      const variant = lt.variants[tag];
      const payload = w
        .slice(i + 1, i + 1 + width)
        .map((x, k) => unpackSlot(variant, k, x));
      const [v] = decodeParam(st, variant, payload, 0);
      return [v, i + 1 + width];
    }
    default:
      throw new Error(`unsupported parameter kind ${lt.k}`);
  }
}

// Union payload slots may be wider than the variant's own lowering: i64 slots carry i32 values
// zero-extended and f64 values as their IEEE-754 bits.
function unpackSlot(variant, k, x) {
  const own = wasmTypes(variant)[k];
  if (typeof x !== 'bigint') return x;
  if (own === 'i32') return Number(BigInt.asIntN(32, x));
  if (own === 'f64') {
    const dv = new DataView(new ArrayBuffer(8));
    dv.setBigInt64(0, x, true);
    return dv.getFloat64(0, true);
  }
  return x;
}

// The Wasm parameter types of an L-type. Union payload types come from the generator (see
// `oneof_payload` in generator/lowering.py).
function wasmTypes(lt) {
  switch (lt.k) {
    case 'bool':
    case 'i32':
    case 'handle':
    case 'callback':
    case 'varargs':
      return ['i32'];
    case 'i64':
      return ['i64'];
    case 'f64':
      return ['f64'];
    case 'string':
    case 'bytes':
      return ['i32', 'i32'];
    case 'maybe':
      if (lt.inner.k === 'handle' || lt.inner.k === 'callback') return ['i32'];
      return ['i32', ...wasmTypes(lt.inner)];
    case 'oneof':
      return ['i32', ...lt.payload];
    default:
      throw new Error(`no wasm types for ${lt.k}`);
  }
}

// ---- Result encoding: writes a JS value into the slot for one L-type. ----

function slotSize(lt) {
  switch (lt.k) {
    case 'bool':
    case 'i32':
    case 'handle':
    case 'callback':
      return 4;
    case 'i64':
    case 'f64':
      return 8;
    case 'string':
    case 'bytes':
      return 16;
    case 'maybe':
      if (lt.inner.k === 'handle') return 4;
      return 8 + round8(slotSize(lt.inner));
    case 'oneof':
      return 8 + round8(Math.max(...lt.variants.map(slotSize)));
    default:
      return 0;
  }
}

function round8(n) {
  return (n + 7) & ~7;
}

// Writes `v` into the slot at `ptr` and returns a JSON-able summary of what was written, for
// traces: numbers as numbers, i64 as "<n>n", strings as text, bytes as {bytes: length},
// handles as {h: handle}, absent optionals as null and unions as {tag, v}.
function writeResult(st, lt, ptr, v) {
  ptr >>>= 0;
  if (lt.k === 'void') return undefined;
  st.checkRange(ptr, slotSize(lt));
  const view = st.view();
  switch (lt.k) {
    case 'bool':
      view.setUint32(ptr, v ? 1 : 0, true);
      return v ? 1 : 0;
    case 'i32':
      view.setInt32(ptr, Number(v) | 0, true);
      return lt.unsigned ? Number(v) >>> 0 : Number(v) | 0;
    case 'i64': {
      const n = BigInt.asIntN(64, BigInt(v));
      view.setBigInt64(ptr, n, true);
      return `${n}n`;
    }
    case 'f64': {
      const n = lt.date ? v.getTime() : Number(v);
      view.setFloat64(ptr, n, true);
      return n;
    }
    case 'string': {
      const text = String(v);
      st.writeBytesSlot(ptr, utf8Encoder.encode(text));
      return text;
    }
    case 'bytes': {
      const b = toBytes(v);
      st.writeBytesSlot(ptr, b);
      return { bytes: b.length };
    }
    case 'handle':
    case 'callback': {
      const h = st.add(v);
      view.setUint32(ptr, h, true);
      return { h };
    }
    case 'maybe':
      if (lt.inner.k === 'handle') {
        const h = isAbsent(v) ? 0 : st.add(v);
        view.setUint32(ptr, h, true);
        return h === 0 ? null : { h };
      }
      if (isAbsent(v)) {
        view.setUint32(ptr, 0, true);
        return null;
      }
      view.setUint32(ptr, 1, true);
      return writeResult(st, lt.inner, ptr + 8, v);
    case 'oneof': {
      const tag = classify(lt, v);
      view.setUint32(ptr, tag, true);
      return { tag, v: writeResult(st, lt.variants[tag], ptr + 8, v) };
    }
    default:
      throw new Error(`unsupported result kind ${lt.k}`);
  }
}

// The tag of a union result is the first variant, in RTTI order, whose class accepts the value.
function classify(lt, v) {
  for (let tag = 0; tag < lt.variants.length; tag++) {
    if (accepts(lt.variants[tag], v)) return tag;
  }
  throw new TypeError('value matches no variant of the union result');
}

function accepts(lt, v) {
  switch (lt.k) {
    case 'void':
      return v === undefined;
    case 'bool':
      return typeof v === 'boolean';
    case 'i32':
    case 'f64':
      return lt.date ? v instanceof Date : typeof v === 'number';
    case 'i64':
      return typeof v === 'bigint' || typeof v === 'number';
    case 'string':
      return typeof v === 'string';
    case 'bytes':
      return v instanceof ArrayBuffer || ArrayBuffer.isView(v);
    case 'handle': {
      if (lt.of === 'unknown') return true;
      if (lt.js === 'Array') return Array.isArray(v);
      const ctor = globalCtor(lt.js);
      if (ctor !== undefined) return v instanceof ctor;
      return (typeof v === 'object' && v !== null) || typeof v === 'function';
    }
    default:
      return false;
  }
}

// ---- Lowered imports: JSG member access. ----

function memberImpl(d) {
  const m = d.m;
  switch (d.t) {
    case 'method':
      return (self, args) => {
        const ctor = globalCtor(d.js);
        const fn =
          ctor !== undefined && m in ctor.prototype
            ? ctor.prototype[m]
            : self[m];
        if (typeof fn !== 'function')
          throw new TypeError(`${d.n}: not a function`);
        return fn.apply(self, args);
      };
    case 'static':
      return (_self, args) => globalThis[d.js][m](...args);
    case 'ctor':
      return (_self, args) => new globalThis[d.js](...args);
    case 'get':
      return (self) => {
        const desc = d.proto ? accessor(d.js, m) : undefined;
        return desc?.get ? desc.get.call(self) : self[m];
      };
    case 'set':
      return (self, args) => {
        const desc = d.proto ? accessor(d.js, m) : undefined;
        if (desc?.set) desc.set.call(self, args[0]);
        else self[m] = args[0];
      };
    case 'global':
      return (_self, args) => globalThis[m](...args);
    default:
      throw new Error(`unknown descriptor kind ${d.t}`);
  }
}

// The accessor of prototype property `m` of global class `js`, so that calling it checks the
// receiver (JSG throws `TypeError: Illegal invocation` for an object of another class).
function accessor(js, m) {
  const ctor = globalCtor(js);
  return ctor === undefined
    ? undefined
    : Object.getOwnPropertyDescriptor(ctor.prototype, m);
}

// Whether the member behind a lowered import exists in this isolate. Imports whose member is
// absent (gated off by a compatibility flag) are left out of the import object, so a module
// that needs them fails to link with an error naming the import.
function memberPresent(d) {
  if (d.t === 'global') return typeof globalThis[d.m] === 'function';
  const ctor = globalCtor(d.js);
  if (ctor === undefined) return true; // Not a global class; cannot check, assume present.
  if (d.t === 'ctor') return true;
  if (d.t === 'static') return d.m in ctor;
  if (d.t === 'get' || d.t === 'set') {
    // JSG defines a property on the prototype, or on each instance, where it cannot be checked
    // without one. That is the case for instance properties, and for all properties at
    // compatibility dates without jsg_property_on_prototype_template, when the prototype has
    // no accessors at all.
    return !d.proto || d.m in ctor.prototype || !hasAccessors(ctor.prototype);
  }
  return d.m in ctor.prototype;
}

function hasAccessors(proto) {
  return Object.values(Object.getOwnPropertyDescriptors(proto)).some(
    (p) => p.get !== undefined
  );
}

function makeLoweredImport(st, d) {
  const impl = memberImpl(d);
  return function (...w) {
    if (st.inInit) throw new GuestFault(`import ${d.n} called during init`);
    let i = 0;
    let self;
    if (d.self) self = st.get(w[i++]);
    const args = [];
    for (const p of d.p) {
      const [v, next] = decodeParam(st, p, w, i);
      if (p.k === 'varargs') args.push(...v);
      else args.push(v);
      i = next;
    }
    const retPtr = d.r.k === 'void' ? 0 : w[i];
    precheckResult(st, d.r, retPtr);
    return invoke(
      st,
      d.s,
      () => impl(self, args),
      (v) => {
        st.lastResult = writeResult(st, d.r, retPtr, v);
      }
    );
  };
}

// Checks the result slot, and the guest buffer named by a StrSlot in it, before the call has
// any effect, so a bad pointer traps before the API runs.
function precheckResult(st, lt, ptr) {
  if (lt.k === 'void') return;
  ptr >>>= 0;
  st.checkRange(ptr, slotSize(lt));
  const offset = strSlotOffset(lt);
  if (offset !== null) {
    const view = st.view();
    st.checkRange(
      view.getUint32(ptr + offset, true),
      view.getUint32(ptr + offset + 4, true)
    );
  }
}

// Offset of the StrSlot within a result slot of L-type `lt`, or null if it has none. Every
// string or bytes variant of a union shares the same offset.
function strSlotOffset(lt) {
  switch (lt.k) {
    case 'string':
    case 'bytes':
      return 0;
    case 'maybe': {
      if (lt.inner.k === 'handle') return null;
      const inner = strSlotOffset(lt.inner);
      return inner === null ? null : 8 + inner;
    }
    case 'oneof':
      return lt.variants.some((v) => strSlotOffset(v) !== null) ? 8 : null;
    default:
      return null;
  }
}

// Calls `body` and reports the outcome as a status code: 0 after `onValue` stored the result,
// or the handle of the thrown value. Suspending imports await the value first.
function invoke(st, suspending, body, onValue) {
  const fail = (e) => {
    if (e instanceof GuestFault) throw e;
    return st.add(e);
  };
  if (suspending) {
    let p;
    try {
      p = Promise.resolve(body());
    } catch (e) {
      return Promise.resolve(fail(e));
    }
    return p.then(
      (v) => {
        onValue(v);
        return 0;
      },
      (e) => fail(e)
    );
  }
  let v;
  try {
    v = body();
  } catch (e) {
    return fail(e);
  }
  onValue(v);
  return 0;
}

// ---- Conventions-defined imports. ----

// The implementations of each module's conventions-defined imports, by module and import name.
// They receive decoded arguments (per the descriptor's `p`) and return the JS result, which the
// wrapper encodes per the descriptor's `ret` and `r`. Handle parameters arrive as the values
// they refer to, except for handle_drop, which receives the raw handle.
const DEFINED = {
  'cf:core/v1': coreV1,
};

function coreV1(st, log) {
  return {
    handle_drop: (h) => {
      st.drop(h);
    },
    value_undefined: () => undefined,
    value_null: () => null,
    value_bool: (v) => v,
    value_f64: (v) => v,
    value_string: (s) => s,
    value_bytes: (b) => b,
    value_kind: (v) => valueKind(v),
    value_to_bool: (v) => !!v,
    value_to_f64: (v) => Number(v),
    value_to_string: (v) => `${v}`,
    value_to_bytes: (v) => toBytes(v),
    object_new: () => ({}),
    object_set: (o, k, v) => {
      o[k] = v;
    },
    object_set_string: (o, k, v) => {
      o[k] = v;
    },
    object_set_f64: (o, k, v) => {
      o[k] = v;
    },
    object_set_bool: (o, k, v) => {
      o[k] = v;
    },
    env_get: (env, name) => (Object.hasOwn(env, name) ? env[name] : undefined),
    array_new: () => [],
    array_push: (a, v) => {
      a.push(v);
    },
    array_len: (a) => {
      if (!Array.isArray(a)) throw new GuestFault('array_len: not an array');
      return a.length;
    },
    array_get: (a, i) => a[i],
    error_new: (name, message) => {
      const e = new Error(message);
      e.name = name;
      return e;
    },
    error_name: (e) => (e instanceof Error ? String(e.name) : ''),
    error_message: (e) => (e instanceof Error ? String(e.message) : String(e)),
    respond: (r) => {
      if (st.onRespond === null) throw new TypeError('respond() called twice');
      if (!(r instanceof Response))
        throw new TypeError('respond() expects a Response');
      const cb = st.onRespond;
      st.onRespond = null;
      cb(r);
    },
    console_log: (level, msg) => {
      const fn =
        [console.debug, console.log, console.warn, console.error][level] ??
        console.log;
      fn.call(console, msg);
      log?.(level, msg);
    },
    panic: (msg) => {
      console.error(`guest panicked: ${msg}`);
      throw new GuestFault(`guest panicked: ${msg}`);
    },
  };
}

function encodeDirect(st, lt, v) {
  switch (lt.k) {
    case 'handle':
      return st.add(v);
    case 'bool':
      return v ? 1 : 0;
    case 'i32':
      return lt.unsigned ? v >>> 0 : v | 0;
    default:
      return v;
  }
}

function makeDefinedImport(st, d, impl) {
  return function (...w) {
    if (st.inInit && !d.init)
      throw new GuestFault(`import ${d.n} called during init`);
    let i = 0;
    const args = [];
    for (const p of d.p) {
      if (d.n === 'handle_drop') {
        args.push(w[i++]);
        continue;
      }
      const [v, next] = decodeParam(st, p, w, i);
      args.push(v);
      i = next;
    }
    if (d.ret === 'none') {
      impl(...args);
      return undefined;
    }
    if (d.ret === 'direct') return encodeDirect(st, d.r, impl(...args));
    const retPtr = d.r.k === 'void' ? 0 : w[i];
    precheckResult(st, d.r, retPtr);
    return invoke(
      st,
      d.s,
      () => impl(...args),
      (v) => {
        st.lastResult = writeResult(st, d.r, retPtr, v);
      }
    );
  };
}

// ---- Import objects. ----

// Records each import call in options.trace: the module and import name, the raw Wasm
// arguments, the text of string arguments, the return value and, for results written to a
// slot, a summary of the result (see writeResult). Traces of the shim and of a native
// implementation must be equal.
function traced(st, module, d, fn) {
  if (st.trace === null) return fn;
  const params = d.self ? [{ k: 'handle' }, ...d.p] : d.p;
  return function (...w) {
    const entry = {
      module,
      import: d.n,
      args: w.map((x) => (typeof x === 'bigint' ? `${x}n` : x)),
    };
    const strings = [];
    let i = 0;
    for (const p of params) {
      if (p.k === 'string') strings.push(st.readString(w[i], w[i + 1]));
      i += wasmTypes(p).length;
    }
    if (strings.length > 0) entry.strings = strings;
    st.trace.push(entry);
    st.lastResult = undefined;
    const finish = (r) => {
      entry.result = r;
      if (st.lastResult !== undefined) entry.value = st.lastResult;
      st.lastResult = undefined;
      return r;
    };
    let r;
    try {
      r = fn(...w);
    } catch (e) {
      entry.fault = String(e?.message ?? e);
      throw e;
    }
    if (r instanceof Promise) {
      return r.then(finish, (e) => {
        entry.fault = String(e?.message ?? e);
        throw e;
      });
    }
    return finish(r);
  };
}

// The import object for one request: one namespace per shim module, all sharing the request's
// handle table.
function buildImports(st, shims, options) {
  const imports = {};
  for (const shim of shims) {
    const ns = {};
    const defined = DEFINED[shim.module]?.(st, options.log);
    for (const d of shim.imports) {
      let fn;
      if (d.t === 'defined') {
        const impl = defined?.[d.n];
        if (impl === undefined)
          throw new Error(
            `shim has no implementation of ${shim.module} ${d.n}`
          );
        fn = makeDefinedImport(st, d, impl);
      } else {
        if (!memberPresent(d)) continue;
        fn = makeLoweredImport(st, d);
      }
      fn = traced(st, shim.module, d, fn);
      ns[d.n] = d.s ? new WebAssembly.Suspending(fn) : fn;
    }
    imports[shim.module] = ns;
  }
  return imports;
}

// ---- Pre-initialized images. ----

// Granularity of the image diff. Any size works; 4 KiB matches the native path's page size.
const IMAGE_PAGE = 4096;

// The image of each module, built once: { pages, chunks: [[offset, bytes]], globals: [[name,
// value]] } or { error }. Building is synchronous, so no promise is shared between requests.
const images = new WeakMap();

// Runs `init` once in a scratch instance and records how its memory and exported globals differ
// from a freshly instantiated module: memory size in pages, the 4 KiB chunks that differ, and the
// exported globals whose values differ. Applying that diff to a fresh instance reproduces the
// state after `init` without running it again.
function buildImage(module, shims, options) {
  const st = new RequestState({ log: options.log });
  const instance = new WebAssembly.Instance(
    module,
    buildImports(st, shims, options)
  );
  const exports = instance.exports;
  if (typeof exports.handle !== 'function')
    throw new TypeError('module does not export handle');
  if (typeof exports.init !== 'function')
    return { pages: 0, chunks: [], globals: [] };
  const before = snapshotState(exports);
  st.instance = instance;
  st.inInit = true;
  try {
    exports.init();
  } finally {
    st.inInit = false;
  }
  const after = snapshotState(exports);
  const chunks = [];
  for (let off = 0; off < after.memory.length; off += IMAGE_PAGE) {
    const end = Math.min(off + IMAGE_PAGE, after.memory.length);
    if (!sameBytes(after.memory, before.memory, off, end)) {
      chunks.push([off, after.memory.slice(off, end)]);
    }
  }
  const globals = after.globals.filter(
    ([name, v]) => !Object.is(before.globalMap.get(name), v)
  );
  return { pages: after.memory.length / 65536, chunks, globals };
}

function snapshotState(exports) {
  const globals = Object.entries(exports)
    .filter(([, v]) => v instanceof WebAssembly.Global)
    .map(([name, g]) => [name, g.value]);
  return {
    memory: new Uint8Array(exports.memory.buffer.slice(0)),
    globals,
    globalMap: new Map(globals),
  };
}

function sameBytes(a, b, start, end) {
  if (end > b.length) {
    for (let i = start; i < end; i++) if (a[i] !== 0) return false;
    return true;
  }
  for (let i = start; i < end; i++) if (a[i] !== b[i]) return false;
  return true;
}

function imageFor(module, shims, options) {
  let image = images.get(module);
  if (image === undefined) {
    try {
      image = buildImage(module, shims, options);
    } catch (error) {
      image = { error };
    }
    images.set(module, image);
  }
  if (image.error !== undefined) throw image.error;
  return image;
}

function applyImage(exports, image) {
  const memory = exports.memory;
  const pages = memory.buffer.byteLength / 65536;
  if (image.pages > pages) memory.grow(image.pages - pages);
  const bytes = new Uint8Array(memory.buffer);
  for (const [off, chunk] of image.chunks) bytes.set(chunk, off);
  for (const [name, v] of image.globals) exports[name].value = v;
}

// ---- Requests. ----

// Runs one request: a fresh instance of the compiled module, the module's pre-initialized image
// (the state after `init`, built once per module) copied into it, then the `handle` export
// through JSPI.
async function runRequest(module, shims, request, env, ctx, options) {
  const image = imageFor(module, shims, options);
  const st = new RequestState(options);
  const instance = new WebAssembly.Instance(
    module,
    buildImports(st, shims, options)
  );
  st.instance = instance;
  const exports = instance.exports;
  applyImage(exports, image);
  const handle = WebAssembly.promising(exports.handle);
  const reqH = st.add(request);
  const envH = st.add(env);
  const ctxH = st.add(ctx);

  return await new Promise((resolve, reject) => {
    let responded = false;
    st.onRespond = (response) => {
      responded = true;
      resolve(response);
    };
    const done = handle(reqH, envH, ctxH).then((rv) => {
      if (responded) {
        if (rv < 0) throw st.get(-rv);
        return;
      }
      st.onRespond = null;
      if (rv > 0) {
        const response = st.get(rv);
        if (!(response instanceof Response)) {
          throw new TypeError(
            'handle returned a handle that is not a Response'
          );
        }
        resolve(response);
      } else if (rv < 0) {
        reject(st.get(-rv));
      } else {
        reject(new Error('handle returned no response'));
      }
    });
    done.catch((e) => {
      if (responded) {
        console.error('Wasm handler failed after responding:', e);
      } else {
        reject(e);
      }
    });
    if (ctx && typeof ctx.waitUntil === 'function')
      ctx.waitUntil(done.catch(() => {}));
  });
}

// Returns a Workers module default export serving `module`, a compiled WebAssembly.Module,
// with the imports of `shims`, the default exports of the shim modules of the Wasm import
// modules the guest uses. `options.trace`, if an array, receives one entry per import call;
// `options.log` receives console_log calls.
export function serve(module, shims, options = {}) {
  const provided = new Set(shims.map((s) => s.module));
  for (const { module: name } of WebAssembly.Module.imports(module)) {
    if (!provided.has(name)) {
      throw new TypeError(`no shim module serves Wasm import module ${name}`);
    }
  }
  return {
    fetch(request, env, ctx) {
      return runRequest(module, shims, request, env, ctx, options);
    },
  };
}

export { GuestFault };
