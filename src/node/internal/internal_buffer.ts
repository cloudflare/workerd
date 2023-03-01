// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//

// Copyright 2018-2023 the Deno authors. All rights reserved. MIT license.
// Copyright Joyent and Node contributors. All rights reserved. MIT license.
// Copyright Feross Aboukhadijeh, and other contributors. All rights reserved. MIT license.

// TODO(soon): Remove this once buffer is out of experimental
import { default as CompatibilityFlags } from 'workerd:compatibility-flags';
if (!CompatibilityFlags.workerdExperimental) {
  throw new Error('node:buffer is experimental.');
}

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import {
  ERR_BUFFER_OUT_OF_BOUNDS,
  ERR_OUT_OF_RANGE,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
  ERR_INVALID_BUFFER_SIZE,
  ERR_UNKNOWN_ENCODING,
} from 'node-internal:internal_errors';

import { default as bufferUtil } from 'node-internal:buffer';

import {
  isAnyArrayBuffer,
  isArrayBufferView,
  isUint8Array,
} from 'node-internal:internal_types';

import {
  normalizeEncoding,
} from 'node-internal:internal_utils';

import {
  validateString,
} from 'node-internal:validators';

// Temporary buffers to convert numbers.
const float32Array = new Float32Array(1);
const uInt8Float32Array = new Uint8Array(float32Array.buffer);
const float64Array = new Float64Array(1);
const uInt8Float64Array = new Uint8Array(float64Array.buffer);

// Check endianness.
float32Array[0] = -1; // 0xBF800000
// Either it is [0, 0, 128, 191] or [191, 128, 0, 0]. It is not possible to
// check this with `os.endianness()` because that is determined at compile time.
export const bigEndian = uInt8Float32Array[3] === 0;

// Node.js caps it's max length at uint32_t max, we are very intentionally more
// conservative here, capping at int32_t max.
export const kMaxLength = 2147483647;
export const kStringMaxLength = 536870888;
const MAX_UINT32 = 2 ** 32;
const kIsBuffer = Symbol('kIsBuffer');

// TODO(soon): Implement inspect
// const customInspectSymbol =
//   typeof Symbol === "function" && typeof Symbol["for"] === "function"
//     ? Symbol["for"]("nodejs.util.inspect.custom")
//     : null;

// const INSPECT_MAX_BYTES = 50;

export const constants = {
  MAX_LENGTH: kMaxLength,
  MAX_STRING_LENGTH: kStringMaxLength,
};

function createBuffer(length: number) : Buffer {
  if (length > kMaxLength) {
    throw new ERR_OUT_OF_RANGE('The given length is invalid', `0 to ${kMaxLength}`, length);
  }
  const buf = new Uint8Array(length);
  Object.setPrototypeOf(buf, Buffer.prototype);
  return buf;
}

type WithImplicitCoercion<T> = | T | { valueOf(): T; };
type StringLike = WithImplicitCoercion<string> | { [Symbol.toPrimitive](hint: "string"): string; };
type ArrayBufferLike = WithImplicitCoercion<ArrayBuffer| SharedArrayBuffer>;
type BufferSource = StringLike|ArrayBufferLike|Uint8Array|ReadonlyArray<number>;
type Buffer = typeof Buffer.prototype;
type FillValue = string|number|ArrayBufferView;

export function Buffer(value: number) : Buffer;
export function Buffer(value: StringLike, encoding?: string) : Buffer;
export function Buffer(value: ArrayBufferLike, byteOffset?: number, length?: number) : Buffer;
export function Buffer(value: Uint8Array|ReadonlyArray<number>,
                       byteOffset?: number,
                       length?: number) : Buffer;
                       export function Buffer(value: StringLike,
                       encoding?: string) : Buffer;
export function Buffer(value: number|BufferSource,
                       encodingOrOffset? : string|number,
                       length?: number) : Buffer {
  if (typeof value === "number") {
    if (typeof encodingOrOffset === "string") {
      throw new ERR_INVALID_ARG_TYPE("string", "string", value);
    }
    return allocUnsafe(value);
  }

  return _from(value, encodingOrOffset, length);
}

Object.setPrototypeOf(Buffer.prototype, Uint8Array.prototype);
Object.setPrototypeOf(Buffer, Uint8Array);

Object.defineProperties(Buffer, {
  // By default, Node.js allocates Buffers from a shared slab allocation
  // as a performance optimization. While this is fast, it's not ideal
  // in our environment as it could potentially be used to leak buffer
  // data across multiple requests. We always want our Buffer instances
  // to be distinct allocations. To signal this, we keep our poolSize
  // always set to 0.
  poolSize: {
    enumerable: true,
    value: 0,
    writable: false,
  }
});

Object.defineProperties(Buffer.prototype, {
  parent: {
    enumerable: true,
    get() {
      if (!Buffer.isBuffer(this)) {
        return void 0;
      }
      return this.buffer;
    },
  },
  offset: {
    enumerable: true,
    get() {
      if (!Buffer.isBuffer(this)) {
        return void 0;
      }
      return this.byteOffset;
    },
  },
  [kIsBuffer]: {
    enumerable: false,
    configurable: false,
    value: true,
  },
});

function _from(value: BufferSource,
               encodingOrOffset? : string|number,
               length?: number) : Buffer {
  if (typeof value === "string") {
    return fromString(value, encodingOrOffset as string | undefined);
  }

  if (typeof value === "object" && value != null) {
    if (isAnyArrayBuffer(value)) {
      return fromArrayBuffer(value as ArrayBufferLike, encodingOrOffset as number, length);
    }

    const valueOf = value?.valueOf();
    if (valueOf != null && valueOf !== value &&
        (typeof valueOf === "string" || typeof valueOf === "object")) {
      return _from(valueOf as BufferSource, encodingOrOffset, length);
    }

    if ((value as ArrayBufferLike|Buffer|Object).length !== undefined ||
         isAnyArrayBuffer((value as ArrayBufferLike|Buffer|Object).buffer)) {
      if (typeof (value as ArrayBufferLike|Buffer|Object).length !== "number") {
        return createBuffer(0);
      }

      return fromArrayLike(value as ArrayBufferLike|Buffer|Object);
    }

    if ((value as ArrayBufferLike|Buffer|Object).type === "Buffer" &&
         Array.isArray((value as ArrayBufferLike|Buffer|Object).data)) {
      return fromArrayLike((value as ArrayBufferLike|Buffer|Object).data);
    }

    const toPrimitive = (value as any)[Symbol.toPrimitive];
    if (typeof toPrimitive === "function") {
      const primitive = toPrimitive("string");
      if (typeof primitive === "string") {
        return fromString(primitive, encodingOrOffset as string | undefined);
      }
    }
  }

  throw new ERR_INVALID_ARG_TYPE(
    "first argument",
    [
      "string",
      "Buffer",
      "TypedArray",
      "ArrayBuffer",
      "SharedArrayBuffer",
      "Array",
      "Array-like Object"
    ],
    value,
  );
}

function from(value: StringLike,
              encoding?: string) : Buffer;
function from(value: ArrayBufferLike,
              byteOffset?: number,
              length?: number) : Buffer;
function from(value: Uint8Array|ReadonlyArray<number>,
              byteOffset?: number,
              length?: number) : Buffer;
function from(value: BufferSource, encodingOrOffset?: string|number, length?: number) {
  return _from(value, encodingOrOffset, length);
};

function fromString(string: StringLike, encoding?: string) {
  if (typeof encoding !== "string" || encoding === "") {
    encoding = "utf8";
  }
  const normalizedEncoding = normalizeEncoding(encoding);
  if (!Buffer.isEncoding(normalizedEncoding)) {
    throw new ERR_UNKNOWN_ENCODING(encoding);
  }

  const ab = bufferUtil.decodeString(`${string}`, normalizedEncoding as string);
  if (ab === undefined) {
    throw new ERR_INVALID_ARG_VALUE('string', string,
      `Unable to decode string using encoding ${encoding}`);
  }
  return fromArrayBuffer(ab, 0, ab.byteLength);
}

function fromArrayLike(array: Uint8Array|ReadonlyArray<number>) {
  const u8 = Uint8Array.from(array);
  return fromArrayBuffer(u8.buffer, u8.byteOffset, u8.byteLength);
}

function fromArrayBuffer(obj: ArrayBufferLike,
                         byteOffset: number,
                         length?: number) {
  // Convert byteOffset to integer
  if (byteOffset === undefined) {
    byteOffset = 0;
  } else {
    byteOffset = +byteOffset;
    if (Number.isNaN(byteOffset)) {
      byteOffset = 0;
    }
  }

  const maxLength = (obj as ArrayBuffer).byteLength - byteOffset;

  if (maxLength < 0) {
    throw new ERR_BUFFER_OUT_OF_BOUNDS("offset");
  }

  if (length === undefined) {
    length = maxLength;
  } else {
    // Convert length to non-negative integer.
    length = +length;
    if (length > 0) {
      if (length > maxLength) {
        throw new ERR_BUFFER_OUT_OF_BOUNDS("length");
      }
    } else {
      length = 0;
    }
  }

  const buffer = new Uint8Array(obj as ArrayBuffer, byteOffset, length);
  Object.setPrototypeOf(buffer, Buffer.prototype);
  return buffer;
}

Buffer.from = from;

function of(...args: number[]) {
  const buf = Buffer.alloc(args.length);
  for (let k = 0; k < args.length; k++)
    buf[k] = args[k];
  return buf;
}

Buffer.of = of;

function alloc(size: number, fill?: FillValue, encoding?: string) : Buffer {
  validateNumber(size, "size");
  if (Number.isNaN(size)) {
    throw new ERR_INVALID_ARG_VALUE.RangeError('size', size);
  }
  if (size >= kMaxLength) {
    throw new ERR_OUT_OF_RANGE("size", `0 to ${kMaxLength}`, size);
  }

  const buffer = createBuffer(size);
  if (fill !== undefined) {
    if (encoding !== undefined) {
      validateString(encoding, 'encoding');
    }
    return buffer.fill(fill, encoding);
  }
  return buffer;
}

Buffer.alloc = alloc;

function allocUnsafe(size: number) : Buffer {
  return alloc(size);
}

Buffer.allocUnsafe = allocUnsafe;
Buffer.allocUnsafeSlow = allocUnsafe;

export function SlowBuffer(length: number) {
  return alloc(+length);
}

Object.setPrototypeOf(SlowBuffer.prototype, Uint8Array.prototype);
Object.setPrototypeOf(SlowBuffer, Uint8Array);

Buffer.isBuffer = function isBuffer(b: unknown) {
  return b != null && (b as any)[kIsBuffer] && b !== Buffer.prototype;
};

export function compare(a: Buffer|Uint8Array, b: Buffer|Uint8Array) {
  if (isInstance(a, Uint8Array)) {
    const buf = a as Uint8Array;
    a = fromArrayBuffer(buf.buffer, buf.byteOffset, buf.byteLength);
  }
  if (isInstance(b, Uint8Array)) {
    const buf = b as Uint8Array;
    b = fromArrayBuffer(buf.buffer, buf.byteOffset, buf.byteLength);
  }
  if (!Buffer.isBuffer(a)) {
    throw new ERR_INVALID_ARG_TYPE('a', ['Buffer', 'Uint8Array'], typeof a);
  }
  if (!Buffer.isBuffer(b)) {
    throw new ERR_INVALID_ARG_TYPE('b', ['Buffer', 'Uint8Array'], typeof b);
  }
  if (a === b) return 0;

  return bufferUtil.compare(a, b);
}

Buffer.compare = compare;

export function isEncoding(encoding: unknown) {
  return typeof encoding === "string" &&
    encoding.length !== 0 &&
    normalizeEncoding(encoding) !== undefined;
};

Buffer.isEncoding = isEncoding;

Buffer.concat = function concat(list: (Buffer|Uint8Array)[], length?: number) {
  if (!Array.isArray(list)) {
    throw new ERR_INVALID_ARG_TYPE("list", "(Buffer|Uint8Array)[]", list);
  }

  if (list.length === 0) return alloc(0);

  if (length === undefined) {
    length = 0;
    for (let i = 0; i < list.length; i++) {
      if (list[i].length !== undefined) {
        length += list[i].length;
      } else {
        throw new ERR_INVALID_ARG_TYPE('list', '(Buffer|Uint8Array)[]', list[i]);
      }
    }
  }
  validateOffset(length, "length");

  const ab = bufferUtil.concat(list, length as number);
  return fromArrayBuffer(ab, 0, length);
};

function base64ByteLength(str: string) {
  let len = str.length;
  if (str.charCodeAt(len - 1) === 0x3d) {
    len--;
  } if (len > 1 && str.charCodeAt(len - 1) === 0x3d)
    len--;

  // Base64 ratio: 3/4
  return (len * 3) >>> 2;
}

function byteLength(string: string|ArrayBufferView|ArrayBuffer|SharedArrayBuffer,
                    encoding?: string) {
  if (typeof string !== "string") {
    if (isArrayBufferView(string) || isAnyArrayBuffer(string)) {
      return string.byteLength;
    }

    throw new ERR_INVALID_ARG_TYPE("string", ["string", "Buffer", "ArrayBuffer"], string);
  }

  string = `${string}`;
  let normalizedEncoding = normalizeEncoding(encoding);
  if (!Buffer.isEncoding(normalizedEncoding)) {
    normalizedEncoding = "utf8";
  }

  switch (normalizedEncoding) {
    case 'ascii':
      // Fall through
    case 'latin1':
      return (string as string).length;
    case 'utf16le':
      return (string as string).length * 2;
    case 'base64':
      // Fall through
    case 'base64url':
      return base64ByteLength(string as string);
    case 'hex':
      return (string as string).length >>> 1;
    case 'utf8':
      // Fall-through
    default:
      return bufferUtil.byteLength(string as string);
  }
}

Buffer.byteLength = byteLength;

Buffer.prototype.swap16 = function swap16() {
  const len = this.length;
  if (len % 2 !== 0) {
    throw new ERR_INVALID_BUFFER_SIZE(16);
  }
  bufferUtil.swap(this, 16);
  return this;
};

Buffer.prototype.swap32 = function swap32() {
  const len = this.length;
  if (len % 4 !== 0) {
    throw new ERR_INVALID_BUFFER_SIZE(32);
  }
  bufferUtil.swap(this, 32);
  return this;
};

Buffer.prototype.swap64 = function swap64() {
  const len = this.length;
  if (len % 8 !== 0) {
    throw new ERR_INVALID_BUFFER_SIZE(64);
  }
  bufferUtil.swap(this, 64);
  return this;
};

Buffer.prototype.toString = function toString(
    encoding?: string,
    start?: number,
    end?: number) {
  if (arguments.length === 0) {
    return bufferUtil.toString(this, 0, this.length, "utf8");
  }

  const len = this.length;

  if (start === undefined || start <= 0) {
    start = 0;
  } else if (start >= len) {
    return "";
  } else {
    start |= 0;
  }

  if (end === undefined || end > len) {
    end = len;
  } else {
    end |= 0;
  }

  if ((end as number) <= start) {
    return "";
  }

  const normalizedEncoding = normalizeEncoding(`${encoding}`);
  if (!Buffer.isEncoding(normalizedEncoding)) {
    throw new ERR_UNKNOWN_ENCODING(encoding as string);
  }

  return bufferUtil.toString(this, start as number, end as number, normalizedEncoding as string);
};

Buffer.prototype.toLocaleString = Buffer.prototype.toString;

Buffer.prototype.equals = function equals(b: Buffer|Uint8Array) {
  return compare(this, b) === 0;
};

// TODO(soon): Implement inspect
// Buffer.prototype.inspect = function inspect() {
//   let str = "";
//   const max = INSPECT_MAX_BYTES;
//   str = this.toString("hex", 0, max).replace(/(.{2})/g, "$1 ").trim();
//   if (this.length > max) {
//     str += " ... ";
//   }
//   return "<Buffer " + str + ">";
// };

// if (customInspectSymbol) {
//   Buffer.prototype[customInspectSymbol] = Buffer.prototype.inspect;
// }

Buffer.prototype.compare = function compare(
  target: Buffer|Uint8Array,
  start?: number,
  end?: number,
  thisStart?: number,
  thisEnd?: number,
) {
  if (isInstance(target, Uint8Array)) {
    target = fromArrayBuffer(target.buffer, target.byteOffset, target.byteLength);
  }
  if (!Buffer.isBuffer(target)) {
    throw new ERR_INVALID_ARG_TYPE("target", ["Buffer", "Uint8Array"], target);
  }

  if (start === undefined) {
    start = 0;
  } else {
    validateOffset(start, "targetStart", 0, kMaxLength);
  }

  if (end === undefined) {
    end = target.length;
  } else {
    validateOffset(end, "targetEnd", 0, target.length);
  }

  if (thisStart === undefined) {
    thisStart = 0;
  } else {
    validateOffset(thisStart as number, "sourceStart", 0, kMaxLength);
  }

  if (thisEnd === undefined) {
    thisEnd = this.length;
  } else {
    validateOffset(thisEnd as number, "sourceEnd", 0, this.length);
  }

  return bufferUtil.compare(this, target, {
    aStart: thisStart as number,
    aEnd: thisEnd as number,
    bStart: start as number,
    bEnd: end as number,
  });
};

function includes(
    this: Buffer,
    val: string|number|Buffer|Uint8Array,
    byteOffset?: number,
    encoding?: string) {
  return this.indexOf(val, byteOffset, encoding) !== -1;
}

Buffer.prototype.includes = includes;


// Finds either the first index of `val` in `buffer` at offset >= `byteOffset`,
// OR the last index of `val` in `buffer` at offset <= `byteOffset`.
//
// Arguments:
// - buffer - a Buffer to search
// - val - a string, Buffer, or number
// - byteOffset - an index into `buffer`; will be clamped to an int32
// - encoding - an optional encoding, relevant if val is a string
// - dir - true for indexOf, false for lastIndexOf
function bidirectionalIndexOf(
  buffer: Uint8Array,
  val: string|number|Buffer|Uint8Array,
  byteOffset: number|string|undefined,
  encoding: string|undefined,
  dir: boolean|undefined) {

  if (Buffer.isBuffer(val) && !isUint8Array(val)) {
    throw new ERR_INVALID_ARG_TYPE('val', ['string', 'number', 'Buffer', 'Uint8Array'], val);
  }

  if (typeof byteOffset === 'string') {
    encoding = byteOffset;
    byteOffset = undefined;
  } else if (byteOffset as number > 0x7fffffff) {
    byteOffset = 0x7fffffff;
  } else if (byteOffset as number < -0x80000000) {
    byteOffset = -0x80000000;
  }
  // Coerce to Number. Values like null and [] become 0.
  byteOffset = +(byteOffset as number);
  // If the offset is undefined, "foo", {}, coerces to NaN, search whole buffer.
  if (Number.isNaN(byteOffset)) {
    byteOffset = dir ? 0 : (buffer.length || buffer.byteLength);
  }
  dir = !!dir;  // Cast to bool.

  if (typeof val === 'number') {
    val = (val >>> 0) & 0xff;
    if (dir) {
      return Uint8Array.prototype.indexOf.call(buffer, val, byteOffset);
    } else {
      return Uint8Array.prototype.lastIndexOf.call(buffer, val, byteOffset);
    }
  }

  if (typeof val !== 'string' && !isUint8Array(val) && !Buffer.isBuffer(val)) {
    throw new ERR_INVALID_ARG_TYPE('value', ['number', 'string', 'Buffer', 'Uint8Array'], val);
  }

  let normalizedEncoding = normalizeEncoding(encoding);
  if (!Buffer.isEncoding(normalizedEncoding)) {
    throw new ERR_UNKNOWN_ENCODING(encoding as string);
  }

  const result = bufferUtil.indexOf(buffer, val, byteOffset, normalizedEncoding, dir);
  return result == null ? -1 : result;
}

Buffer.prototype.indexOf = function indexOf(
    val: string|number|Buffer|Uint8Array,
    byteOffset?: number|string,
    encoding?: string) {
  return bidirectionalIndexOf(this, val, byteOffset, encoding, true);
};

Buffer.prototype.lastIndexOf = function lastIndexOf(
    val: string|number|Buffer|Uint8Array,
    byteOffset?: number|string,
    encoding?: string) {
  return bidirectionalIndexOf(this, val, byteOffset, encoding, false);
};

Buffer.prototype.asciiSlice = function asciiSlice(offset: number, length: number) {
  validateOffset(offset, "offset", 0, this.length);
  validateOffset(length, "length", 0, this.length - offset);
  return bufferUtil.toString(this, offset, offset + length, 'ascii');
};

Buffer.prototype.base64Slice = function base64Slice(offset: number, length: number) {
  validateOffset(offset, "offset", 0, this.length);
  validateOffset(length, "length", 0, this.length - offset);
  return bufferUtil.toString(this, offset, offset + length, 'base64');
};

Buffer.prototype.base64urlSlice = function base64urlSlice(offset: number, length: number) {
  validateOffset(offset, "offset", 0, this.length);
  validateOffset(length, "length", 0, this.length - offset);
  return bufferUtil.toString(this, offset, offset + length, 'base64url');
};

Buffer.prototype.hexSlice = function hexSlice(offset: number, length: number) {
  validateOffset(offset, "offset", 0, this.length);
  validateOffset(length, "length", 0, this.length - offset);
  return bufferUtil.toString(this, offset, offset + length, 'hex');
};

Buffer.prototype.latin1Slice = function latin1Slice(offset: number,
                                                    length: number) {
  validateOffset(offset, "offset", 0, this.length);
  validateOffset(length, "length", 0, this.length - offset);
  return bufferUtil.toString(this, offset, offset + length, 'latin1');
};

Buffer.prototype.ucs2Slice = function ucs2Slice(offset: number, length: number) {
  validateOffset(offset, "offset", 0, this.length);
  validateOffset(length, "length", 0, this.length - offset);
  return bufferUtil.toString(this, offset, offset + length, 'utf16le');
};

Buffer.prototype.utf8Slice = function utf8Slice(offset: number, length: number) {
  validateOffset(offset, "offset", 0, this.length);
  validateOffset(length, "length", 0, this.length - offset);
  return bufferUtil.toString(this, offset, offset + length, 'utf8');
};

Buffer.prototype.asciiWrite = function asciiWrite(string: StringLike,
                                                  offset?: number,
                                                  length?: number) {
  offset ??= 0;
  length ??= this.length;
  validateOffset(offset as number, "offset", 0, this.length);
  validateOffset(length as number, "length", 0, this.length - offset);
  return bufferUtil.write(this, `${string}`, offset as number, length as number, 'ascii');
};

Buffer.prototype.base64Write = function base64Write(string: StringLike,
                                                    offset?: number,
                                                    length?: number) {
  offset ??= 0;
  length ??= this.length;
  validateOffset(offset as number, "offset", 0, this.length);
  validateOffset(length as number, "length", 0, this.length - offset);
  return bufferUtil.write(this, `${string}`, offset as number, length as number, 'base64');
};

Buffer.prototype.base64urlWrite = function base64urlWrite(string: StringLike,
                                                          offset?: number,
                                                          length?: number) {
  offset ??= 0;
  length ??= this.length;
  validateOffset(offset as number, "offset", 0, this.length);
  validateOffset(length as number, "length", 0, this.length - offset);
  return bufferUtil.write(this, `${string}`, offset as number, length as number, 'base64url');
};

Buffer.prototype.hexWrite = function hexWrite(string: StringLike,
                                              offset: number,
                                              length: number) {
  offset ??= 0;
  length ??= this.length;
  validateOffset(offset as number, "offset", 0, this.length);
  validateOffset(length as number, "length", 0, this.length - offset);
  return bufferUtil.write(this, `${string}`, offset as number, length as number, 'hex');
};

Buffer.prototype.latin1Write = function latin1Write(string: StringLike,
                                                    offset: number,
                                                    length: number) {
  offset ??= 0;
  length ??= this.length;
  validateOffset(offset as number, "offset", 0, this.length);
  validateOffset(length as number, "length", 0, this.length - offset);
  return bufferUtil.write(this, `${string}`, offset as number, length as number, 'latin1');
};

Buffer.prototype.ucs2Write = function ucs2Write(string: StringLike,
                                                offset: number,
                                                length: number) {
  offset ??= 0;
  length ??= this.length;
  validateOffset(offset as number, "offset", 0, this.length);
  validateOffset(length as number, "length", 0, this.length - offset);
  return bufferUtil.write(this, `${string}`, offset as number, length as number, 'utf16le');
};

Buffer.prototype.utf8Write = function utf8Write(string: StringLike,
                                                offset: number,
                                                length: number) {
  offset ??= 0;
  length ??= this.length;
  validateOffset(offset as number, "offset", 0, this.length);
  validateOffset(length as number, "length", 0, this.length - offset);
  return bufferUtil.write(this, `${string}`, offset as number, length as number, 'utf8');
};

Buffer.prototype.write = function write(string: StringLike,
                                        offset?: number | string,
                                        length?: number | string,
                                        encoding?: string) {
  string = `${string}`;
  if (offset === undefined) {
    // Buffer#write(string)
    return bufferUtil.write(this, string as string, 0, this.length, "utf8");
  }

  if (length === undefined && typeof offset === 'string') {
    // Buffer#write(string, encoding)
    encoding = offset;
    length = this.length;
    offset = 0;
  } else {
    // Buffer#write(string, offset[, length][, encoding])
    validateOffset(offset as number, 'offset', 0, this.length);

    const remaining = this.length - (offset as number);

    if (length === undefined) {
      length = remaining;
    } else if (typeof length === 'string') {
      encoding = length;
      length = remaining;
    } else {
      validateOffset(length, 'length', 0, this.length);
      if (length > remaining) {
        length = remaining;
      }
    }
  }

  if (!encoding) {
    return bufferUtil.write(this, string as string, offset as number, length as number, "utf8");
  }

  const normalizedEncoding = normalizeEncoding(encoding);
  if (!Buffer.isEncoding(normalizedEncoding)) {
    throw new ERR_UNKNOWN_ENCODING(encoding as string);
  }

  return bufferUtil.write(this, string as string, offset as number, length as number,
                          normalizedEncoding as string);
};

Buffer.prototype.toJSON = function toJSON() {
  return {
    type: "Buffer",
    data: Array.prototype.slice.call(this._arr || this, 0),
  };
};

Buffer.prototype.slice = function slice(start: number, end?: number) {
  const len = this.length;
  start = ~~start;
  end = end === void 0 ? len : ~~end;
  if (start < 0) {
    start += len;
    if (start < 0) {
      start = 0;
    }
  } else if (start > len) {
    start = len;
  }
  if (end === undefined) {
    end = this.byteLength;
  } else if (end < 0) {
    end += len;
    if (end < 0) {
      end = 0;
    }
  } else if (end > len) {
    end = len;
  }
  if ((end as number) < start) {
    end = start;
  }
  const newBuf = this.subarray(start, end);
  Object.setPrototypeOf(newBuf, Buffer.prototype);
  return newBuf;
};

Buffer.prototype.readUintLE = Buffer.prototype.readUIntLE = function readUIntLE(
  offset: number,
  byteLength: number) {
  if (offset === undefined) {
    throw new ERR_INVALID_ARG_TYPE("offset", "number", offset);
  }
  switch (byteLength) {
    case 1: return this.readUInt8(offset);
    case 2: return this.readUInt16LE(offset);
    case 3: return readUInt24LE(this, offset);
    case 4: return this.readUInt32LE(offset);
    case 5: return readUInt40LE(this, offset);
    case 6: return readUInt48LE(this, offset);
    default:
      boundsError(byteLength, 6, "byteLength");
  }
};

Buffer.prototype.readUintBE = Buffer.prototype.readUIntBE = function readUIntBE(
  offset: number,
  byteLength: number) {
  if (offset === undefined) {
    throw new ERR_INVALID_ARG_TYPE("offset", "number", offset);
  }
  switch (byteLength) {
    case 1: return this.readUInt8(offset);
    case 2: return this.readUInt16BE(offset);
    case 3: return readUInt24BE(this, offset);
    case 4: return this.readUInt32BE(offset);
    case 5: return readUInt40BE(this, offset);
    case 6: return readUInt48BE(this, offset);
    default:
      boundsError(byteLength, 6, "byteLength");
  }
};

Buffer.prototype.readUint8 = Buffer.prototype.readUInt8 = function readUInt8(
  offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const val = this[offset];
  if (val === undefined) {
    boundsError(offset, this.length - 1);
  }

  return val;
};

Buffer.prototype.readUint16BE = Buffer.prototype.readUInt16BE = readUInt16BE;

Buffer.prototype.readUint16LE =
  Buffer.prototype.readUInt16LE =
    function readUInt16LE(offset: number = 0) {
      validateOffset(offset, "offset", 0, this.length);
      const first = this[offset];
      const last = this[offset + 1];
      if (first === undefined || last === undefined) {
        boundsError(offset, this.length - 2);
      }

      return first + last * 2 ** 8;
    };

Buffer.prototype.readUint32LE =
  Buffer.prototype.readUInt32LE =
    function readUInt32LE(this: Buffer, offset: number = 0) {
      validateOffset(offset, "offset", 0, this.length);
      const first = this[offset];
      const last = this[offset + 3];
      if (first === undefined || last === undefined) {
        boundsError(offset, this.length - 4);
      }

      return first +
        this[++offset] * 2 ** 8 +
        this[++offset] * 2 ** 16 +
        last * 2 ** 24;
    };

Buffer.prototype.readUint32BE = Buffer.prototype.readUInt32BE = readUInt32BE;

Buffer.prototype.readBigUint64LE =
  Buffer.prototype.readBigUInt64LE =
    function readBigUInt64LE(this: Buffer, offset: number = 0) {
      offset = offset >>> 0;
      validateOffset(offset, "offset", 0, this.length);
      const first = this[offset];
      const last = this[offset + 7];
      if (first === undefined || last === undefined) {
        boundsError(offset, this.length - 8);
      }
      const lo = first + this[++offset] * 2 ** 8 +
        this[++offset] * 2 ** 16 +
        this[++offset] * 2 ** 24;
      const hi = this[++offset] + this[++offset] * 2 ** 8 +
        this[++offset] * 2 ** 16 + last * 2 ** 24;
      return BigInt(lo) + (BigInt(hi) << BigInt(32));
    };

Buffer.prototype.readBigUint64BE =
  Buffer.prototype.readBigUInt64BE =
    function readBigUInt64BE(this: Buffer, offset: number = 0) {
      offset = offset >>> 0;
      validateOffset(offset, "offset", 0, this.length);
      const first = this[offset];
      const last = this[offset + 7];
      if (first === undefined || last === undefined) {
        boundsError(offset, this.length - 8);
      }
      const hi = first * 2 ** 24 + this[++offset] * 2 ** 16 +
        this[++offset] * 2 ** 8 + this[++offset];
      const lo = this[++offset] * 2 ** 24 + this[++offset] * 2 ** 16 +
        this[++offset] * 2 ** 8 + last;
      return (BigInt(hi) << BigInt(32)) + BigInt(lo);
    };

Buffer.prototype.readIntLE = function readIntLE(
  offset: number,
  byteLength: number,
) {
  if (offset === undefined) {
    throw new ERR_INVALID_ARG_TYPE("offset", "number", offset);
  }
  switch (byteLength) {
    case 1: return this.readInt8(offset);
    case 2: return this.readInt16LE(offset);
    case 3: return readInt24LE(this, offset);
    case 4: return this.readInt32LE(offset);
    case 5: return readInt40LE(this, offset);
    case 6: return readInt48LE(this, offset);
    default:
      boundsError(byteLength, 6, "byteLength");
  }
};

Buffer.prototype.readIntBE = function readIntBE(
    offset: number,
    byteLength: number) {
  if (offset === undefined) {
    throw new ERR_INVALID_ARG_TYPE("offset", "number", offset);
  }
  switch (byteLength) {
    case 1: return this.readInt8(offset);
    case 2: return this.readInt16BE(offset);
    case 3: return readInt24BE(this, offset);
    case 4: return this.readInt32BE(offset);
    case 5: return readInt40BE(this, offset);
    case 6: return readInt48BE(this, offset);
    default:
      boundsError(byteLength, 6, "byteLength");
  }
};

Buffer.prototype.readInt8 = function readInt8(offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const val = this[offset];
  if (val === undefined) {
    boundsError(offset, this.length - 1);
  }

  return val | (val & 2 ** 7) * 0x1fffffe;
};

Buffer.prototype.readInt16LE = function readInt16LE(offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 1];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 2);
  }

  const val = first + last * 2 ** 8;
  return val | (val & 2 ** 15) * 0x1fffe;
};

Buffer.prototype.readInt16BE = function readInt16BE(offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 1];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 2);
  }

  const val = first * 2 ** 8 + last;
  return val | (val & 2 ** 15) * 0x1fffe;
};

Buffer.prototype.readInt32LE = function readInt32LE(offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 3];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 4);
  }

  return first +
    this[++offset] * 2 ** 8 +
    this[++offset] * 2 ** 16 +
    (last << 24); // Overflow
};

Buffer.prototype.readInt32BE = function readInt32BE(offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 3];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 4);
  }

  return (first << 24) + // Overflow
    this[++offset] * 2 ** 16 +
    this[++offset] * 2 ** 8 +
    last;
};

Buffer.prototype.readBigInt64LE = function readBigInt64LE(this: Buffer, offset: number = 0) {
  offset = offset >>> 0;
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 7];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 8);
  }
  const val = this[offset + 4] + this[offset + 5] * 2 ** 8 +
    this[offset + 6] * 2 ** 16 + (last << 24);
  return (BigInt(val) << BigInt(32)) +
    BigInt(
      first + this[++offset] * 2 ** 8 + this[++offset] * 2 ** 16 +
        this[++offset] * 2 ** 24,
    );
};

Buffer.prototype.readBigInt64BE = function readBigInt64BE(this: Buffer, offset: number = 0) {
  offset = offset >>> 0;
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 7];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 8);
  }
  const val = (first << 24) + this[++offset] * 2 ** 16 +
    this[++offset] * 2 ** 8 + this[++offset];
  return (BigInt(val) << BigInt(32)) +
    BigInt(
      this[++offset] * 2 ** 24 + this[++offset] * 2 ** 16 +
        this[++offset] * 2 ** 8 + last,
    );
};

Buffer.prototype.readFloatLE = function readFloatLE(offset: number = 0) {
  return bigEndian
    ? readFloatBackwards(this, offset)
    : readFloatForwards(this, offset);
};

Buffer.prototype.readFloatBE = function readFloatBE(offset: number = 0) {
  return bigEndian
    ? readFloatForwards(this, offset)
    : readFloatBackwards(this, offset);
};

Buffer.prototype.readDoubleLE = function readDoubleLE(offset: number = 0) {
  return bigEndian
    ? readDoubleBackwards(this, offset)
    : readDoubleForwards(this, offset);
};

Buffer.prototype.readDoubleBE = function readDoubleBE(offset: number = 0) {
  return bigEndian
    ? readDoubleForwards(this, offset)
    : readDoubleBackwards(this, offset);
};

Buffer.prototype.writeUintLE =
  Buffer.prototype.writeUIntLE =
    function writeUIntLE(value: number, offset: number, byteLength: number) {
      switch (byteLength) {
        case 1: return writeU_Int8(this, value, offset, 0, 0xff);
        case 2: return writeU_Int16LE(this, value, offset, 0, 0xffff);
        case 3: return writeU_Int24LE(this, value, offset, 0, 0xffffff);
        case 4: return writeU_Int32LE(this, value, offset, 0, 0xffffffff);
        case 5: return writeU_Int40LE(this, value, offset, 0, 0xffffffffff);
        case 6: return writeU_Int48LE(this, value, offset, 0, 0xffffffffffff);
        default:
          boundsError(byteLength, 6, "byteLength");
      }
    };

Buffer.prototype.writeUintBE =
  Buffer.prototype.writeUIntBE =
    function writeUIntBE(value: number, offset: number, byteLength: number) {
      switch (byteLength) {
        case 1: return writeU_Int8(this, value, offset, 0, 0xff);
        case 2: return writeU_Int16BE(this, value, offset, 0, 0xffff);
        case 3: return writeU_Int24BE(this, value, offset, 0, 0xffffff);
        case 4: return writeU_Int32BE(this, value, offset, 0, 0xffffffff);
        case 5: return writeU_Int40BE(this, value, offset, 0, 0xffffffffff);
        case 6: return writeU_Int48BE(this, value, offset, 0, 0xffffffffffff);
        default:
          boundsError(byteLength, 6, "byteLength");
      }
    };

Buffer.prototype.writeUint8 = Buffer.prototype.writeUInt8 = function writeUInt8(
  value: number,
  offset: number = 0,
) {
  return writeU_Int8(this, value, offset, 0, 0xff);
};

Buffer.prototype.writeUint16LE =
  Buffer.prototype.writeUInt16LE =
    function writeUInt16LE(value: number, offset: number = 0) {
      return writeU_Int16LE(this, value, offset, 0, 0xffff);
    };

Buffer.prototype.writeUint16BE =
  Buffer.prototype.writeUInt16BE =
    function writeUInt16BE(value: number, offset: number = 0) {
      return writeU_Int16BE(this, value, offset, 0, 0xffff);
    };

Buffer.prototype.writeUint32LE =
  Buffer.prototype.writeUInt32LE =
    function writeUInt32LE(value: number, offset: number = 0) {
      return _writeUInt32LE(this, value, offset, 0, 0xffffffff);
    };

Buffer.prototype.writeUint32BE =
  Buffer.prototype.writeUInt32BE =
    function writeUInt32BE(value: number, offset: number = 0) {
      return _writeUInt32BE(this, value, offset, 0, 0xffffffff);
    };

function wrtBigUInt64LE(
    buf: Buffer,
    value: bigint,
    offset: number,
    min: bigint,
    max: bigint) {
  checkIntBI(value, min, max, buf, offset, 7);
  let lo = Number(value & BigInt(4294967295));
  buf[offset++] = lo;
  lo = lo >> 8;
  buf[offset++] = lo;
  lo = lo >> 8;
  buf[offset++] = lo;
  lo = lo >> 8;
  buf[offset++] = lo;
  let hi = Number(value >> BigInt(32) & BigInt(4294967295));
  buf[offset++] = hi;
  hi = hi >> 8;
  buf[offset++] = hi;
  hi = hi >> 8;
  buf[offset++] = hi;
  hi = hi >> 8;
  buf[offset++] = hi;
  return offset;
}

function wrtBigUInt64BE(
    buf: Buffer,
    value: bigint,
    offset: number,
    min: bigint,
    max: bigint) {
  checkIntBI(value, min, max, buf, offset, 7);
  let lo = Number(value & BigInt(4294967295));
  buf[offset + 7] = lo;
  lo = lo >> 8;
  buf[offset + 6] = lo;
  lo = lo >> 8;
  buf[offset + 5] = lo;
  lo = lo >> 8;
  buf[offset + 4] = lo;
  let hi = Number(value >> BigInt(32) & BigInt(4294967295));
  buf[offset + 3] = hi;
  hi = hi >> 8;
  buf[offset + 2] = hi;
  hi = hi >> 8;
  buf[offset + 1] = hi;
  hi = hi >> 8;
  buf[offset] = hi;
  return offset + 8;
}

Buffer.prototype.writeBigUint64LE =
  Buffer.prototype.writeBigUInt64LE =
    function writeBigUInt64LE(this: Buffer, value: bigint, offset: number = 0) {
      return wrtBigUInt64LE(this, value, offset, 0n, 0xffffffffffffffffn);
    };

Buffer.prototype.writeBigUint64BE =
  Buffer.prototype.writeBigUInt64BE =
    function writeBigUInt64BE(this: Buffer, value: bigint, offset: number = 0) {
      return wrtBigUInt64BE(this, value, offset, 0n, 0xffffffffffffffffn);
    };

Buffer.prototype.writeIntLE = function writeIntLE(
    value: number,
    offset: number,
    byteLength: number) {
  switch(byteLength) {
    case 1: return writeU_Int8(this, value, offset, -0x80, 0x7f);
    case 2: return writeU_Int16LE(this, value, offset, -0x8000, 0x7fff);
    case 3: return writeU_Int24LE(this, value, offset, -0x800000, 0x7fffff);
    case 4: return writeU_Int32LE(this, value, offset, -0x80000000, 0x7fffffff);
    case 5: return writeU_Int40LE(this, value, offset, -0x8000000000, 0x7fffffffff);
    case 6: return writeU_Int48LE(this, value, offset, -0x800000000000, 0x7fffffffffff);
    default:
      boundsError(byteLength, 6, "byteLength");
  }
};

Buffer.prototype.writeIntBE = function writeIntBE(
    value: number,
    offset: number,
    byteLength: number) {
  switch(byteLength) {
    case 1: return writeU_Int8(this, value, offset, -0x80, 0x7f);
    case 2: return writeU_Int16BE(this, value, offset, -0x8000, 0x7fff);
    case 3: return writeU_Int24BE(this, value, offset, -0x800000, 0x7fffff);
    case 4: return writeU_Int32BE(this, value, offset, -0x80000000, 0x7fffffff);
    case 5: return writeU_Int40BE(this, value, offset, -0x8000000000, 0x7fffffffff);
    case 6: return writeU_Int48BE(this, value, offset, -0x800000000000, 0x7fffffffffff);
    default:
      boundsError(byteLength, 6, "byteLength");
  }
};

Buffer.prototype.writeInt8 = function writeInt8(value: number, offset: number = 0) {
  return writeU_Int8(this, value, offset, -0x80, 0x7f);
};

Buffer.prototype.writeInt16LE = function writeInt16LE(value: number, offset: number = 0) {
  return writeU_Int16LE(this, value, offset, -0x8000, 0x7fff);
};

Buffer.prototype.writeInt16BE = function writeInt16BE(
  value: number,
  offset: number = 0,
) {
  return writeU_Int16BE(this, value, offset, -0x8000, 0x7fff);
};

Buffer.prototype.writeInt32LE = function writeInt32LE(value: number, offset: number = 0) {
  return writeU_Int32LE(this, value, offset, -0x80000000, 0x7fffffff);
};

Buffer.prototype.writeInt32BE = function writeInt32BE(value: number, offset: number = 0) {
  return writeU_Int32BE(this, value, offset, -0x80000000, 0x7fffffff);
};

Buffer.prototype.writeBigInt64LE =
  function writeBigInt64LE(this: Buffer, value: bigint, offset: number = 0) {
    return wrtBigUInt64LE(this, value, offset, -0x8000000000000000n, 0x7fffffffffffffffn);
  };

Buffer.prototype.writeBigInt64BE =
  function writeBigInt64BE(this: Buffer, value: bigint, offset: number = 0) {
    return wrtBigUInt64BE(this, value, offset, -0x8000000000000000n, 0x7fffffffffffffffn);
  };

Buffer.prototype.writeFloatLE = function writeFloatLE(
  value: number,
  offset: number,
) {
  return bigEndian
    ? writeFloatBackwards(this, value, offset)
    : writeFloatForwards(this, value, offset);
};

Buffer.prototype.writeFloatBE = function writeFloatBE(
  value: number,
  offset: number,
) {
  return bigEndian
    ? writeFloatForwards(this, value, offset)
    : writeFloatBackwards(this, value, offset);
};

Buffer.prototype.writeDoubleLE = function writeDoubleLE(
  value: number,
  offset: number,
) {
  return bigEndian
    ? writeDoubleBackwards(this, value, offset)
    : writeDoubleForwards(this, value, offset);
};

Buffer.prototype.writeDoubleBE = function writeDoubleBE(
  value: number,
  offset: number,
) {
  return bigEndian
    ? writeDoubleForwards(this, value, offset)
    : writeDoubleBackwards(this, value, offset);
};

Buffer.prototype.copy = function copy(
  target: Buffer|Uint8Array,
  targetStart?: number,
  sourceStart?: number,
  sourceEnd?: number,
) {
  if (!isUint8Array(target)) {
    throw new ERR_INVALID_ARG_TYPE("target", ["Buffer", "Uint8Array"], target);
  }

  targetStart = toInteger(targetStart, 0);
  if ((targetStart as number) < 0) {
    throw new ERR_OUT_OF_RANGE("targetStart", ">= 0", targetStart);
  }

  sourceStart = toInteger(sourceStart, 0);
  if ((sourceStart as number) < 0) {
    throw new ERR_OUT_OF_RANGE("sourceStart", ">= 0", sourceStart);
  }
  if ((sourceStart as number) >= MAX_UINT32) {
    throw new ERR_OUT_OF_RANGE("sourceStart", `< ${MAX_UINT32}`, sourceStart);
  }

  sourceEnd ??= this.length;
  sourceEnd = toInteger(sourceEnd, 0);
  if ((sourceEnd as number) < 0) {
    throw new ERR_OUT_OF_RANGE("sourceEnd", ">= 0", sourceEnd);
  }
  if ((sourceEnd as number) >= MAX_UINT32) {
    throw new ERR_OUT_OF_RANGE("sourceEnd", `< ${MAX_UINT32}`, sourceEnd);
  }

  if ((targetStart as number) >= target.length) {
    return 0;
  }

  if ((sourceEnd as number) > 0 && (sourceEnd as number) < (sourceStart as number)) {
    sourceEnd = sourceStart;
  }
  if (sourceEnd === sourceStart) {
    return 0;
  }
  if (target.length === 0 || this.length === 0) {
    return 0;
  }

  if ((sourceEnd as number) > this.length) {
    sourceEnd = this.length;
  }

  if (target.length - (targetStart as number) < (sourceEnd as number) - (sourceStart as number)) {
    sourceEnd = target.length - (targetStart as number) + (sourceStart as number);
  }

  const len = (sourceEnd as number) - (sourceStart as number);
  if (this === target) {
    this.copyWithin(targetStart as number, sourceStart as number, sourceEnd as number);
  } else {
    const sub = this.subarray(sourceStart, sourceEnd);
    target.set(sub, targetStart);
  }

  return len;
};

Buffer.prototype.fill = function fill(
    val: string|number|Buffer|Uint8Array,
    start?: number|string,
    end?: number,
    encoding?: string) {
  let normalizedEncoding : string | undefined;
  if (typeof val === "string") {
    if (typeof start === "string") {
      encoding = start;
      start = 0;
      end = this.length;
    } else if (typeof end === "string") {
      encoding = end;
      end = this.length;
    }
    normalizedEncoding = normalizeEncoding(encoding);
    if (!Buffer.isEncoding(normalizedEncoding)) {
      throw new ERR_UNKNOWN_ENCODING(encoding as string);
    }
    if (val.length === 1) {
      const code = val.charCodeAt(0);
      if (encoding === "utf8" && code < 128 || encoding === "latin1") {
        val = code;
      }
    }
  }

  if (start !== undefined) {
    validateNumber(start, 'start');
  }
  if (end !== undefined) {
    validateNumber(end, 'end');
  }

  if ((end as number) < 0 || (end as number) > this.length) {
    throw new ERR_OUT_OF_RANGE('end', `0 to ${this.length}`, end);
  }
  if ((start as number) < 0 || this.length < (start as number) || this.length < (end as number)) {
    throw new ERR_OUT_OF_RANGE('start', '0 to end', start);
  }
  if ((end as number) <= (start as number)) {
    return this;
  }
  start = (start as number) >>> 0;
  end = end === void 0 ? this.length : end >>> 0;

  if (typeof val === "string") {
    bufferUtil.fillImpl(this,
                        val as string,
                        start as number,
                        end as number,
                        normalizedEncoding);
    return this;
  }

  if (isArrayBufferView(val)) {
    if (val.byteLength === 0) {
      throw new ERR_INVALID_ARG_VALUE('value', 'zero-length');
    }
    bufferUtil.fillImpl(this, val as ArrayBufferView, start as number, end as number);
    return this;
  }

  if (typeof val === "number") {
    val = val & 255;
  } else if (typeof val === "boolean") {
    val = Number(val);
  }
  val ??= 0;

  Uint8Array.prototype.fill.call(this, val, start, end);

  return this;
};

function checkBounds(
    buf: Buffer,
    offset: number,
    byteLength2: number) {
  validateOffset(offset, "offset", 0, buf.length);
  if (buf[offset] === undefined || buf[offset + byteLength2] === undefined) {
    boundsError(offset, buf.length - (byteLength2 + 1));
  }
}

function checkIntBI(
    value: bigint|number,
    min: bigint|number,
    max: bigint|number,
    buf: Buffer,
    offset: number,
    byteLength2: number) {
  if (value > max || value < min) {
    const n = typeof min === "bigint" ? "n" : "";
    let range;
    if (byteLength2 > 3) {
      if (min === 0 || min === BigInt(0)) {
        range = `>= 0${n} and < 2${n} ** ${(byteLength2 + 1) * 8}${n}`;
      } else {
        range = `>= -(2${n} ** ${(byteLength2 + 1) * 8 - 1}${n}) and < 2 ** ${
          (byteLength2 + 1) * 8 - 1
        }${n}`;
      }
    } else {
      range = `>= ${min}${n} and <= ${max}${n}`;
    }
    throw new ERR_OUT_OF_RANGE("value", range, value);
  }
  checkBounds(buf, offset, byteLength2);
}

function isInstance(obj: unknown, type: Function) {
  return obj instanceof type ||
    obj != null && obj.constructor != null &&
      obj.constructor.name != null && obj.constructor.name === type.name;
}

function readUInt48LE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 5];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 6);
  }

  return first +
    buf[++offset] * 2 ** 8 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 24 +
    (buf[++offset] + last * 2 ** 8) * 2 ** 32;
}

function readUInt40LE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 4];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 5);
  }

  return first +
    buf[++offset] * 2 ** 8 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 24 +
    last * 2 ** 32;
}

function readUInt24LE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 2];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 3);
  }

  return first + buf[++offset] * 2 ** 8 + last * 2 ** 16;
}

function readUInt48BE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 5];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 6);
  }

  return (first * 2 ** 8 + buf[++offset]) * 2 ** 32 +
    buf[++offset] * 2 ** 24 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 8 +
    last;
}

function readUInt40BE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 4];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 5);
  }

  return first * 2 ** 32 +
    buf[++offset] * 2 ** 24 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 8 +
    last;
}

function readUInt24BE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 2];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 3);
  }

  return first * 2 ** 16 + buf[++offset] * 2 ** 8 + last;
}

function readUInt16BE(this: Buffer, offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 1];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 2);
  }

  return first * 2 ** 8 + last;
}

function readUInt32BE(this: Buffer, offset: number = 0) {
  validateOffset(offset, "offset", 0, this.length);
  const first = this[offset];
  const last = this[offset + 3];
  if (first === undefined || last === undefined) {
    boundsError(offset, this.length - 4);
  }

  return first * 2 ** 24 +
    this[++offset] * 2 ** 16 +
    this[++offset] * 2 ** 8 +
    last;
}

function readDoubleBackwards(buffer: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buffer.length);
  const first = buffer[offset];
  const last = buffer[offset + 7];
  if (first === undefined || last === undefined) {
    boundsError(offset, buffer.length - 8);
  }

  uInt8Float64Array[7] = first;
  uInt8Float64Array[6] = buffer[++offset];
  uInt8Float64Array[5] = buffer[++offset];
  uInt8Float64Array[4] = buffer[++offset];
  uInt8Float64Array[3] = buffer[++offset];
  uInt8Float64Array[2] = buffer[++offset];
  uInt8Float64Array[1] = buffer[++offset];
  uInt8Float64Array[0] = last;
  return float64Array[0];
}

function readDoubleForwards(buffer: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buffer.length);
  const first = buffer[offset];
  const last = buffer[offset + 7];
  if (first === undefined || last === undefined) {
    boundsError(offset, buffer.length - 8);
  }

  uInt8Float64Array[0] = first;
  uInt8Float64Array[1] = buffer[++offset];
  uInt8Float64Array[2] = buffer[++offset];
  uInt8Float64Array[3] = buffer[++offset];
  uInt8Float64Array[4] = buffer[++offset];
  uInt8Float64Array[5] = buffer[++offset];
  uInt8Float64Array[6] = buffer[++offset];
  uInt8Float64Array[7] = last;
  return float64Array[0];
}

function writeDoubleForwards(buffer: Buffer|Uint8Array, val: number, offset: number = 0) {
  val = +val;
  checkBounds(buffer, offset, 7);

  float64Array[0] = val;
  buffer[offset++] = uInt8Float64Array[0];
  buffer[offset++] = uInt8Float64Array[1];
  buffer[offset++] = uInt8Float64Array[2];
  buffer[offset++] = uInt8Float64Array[3];
  buffer[offset++] = uInt8Float64Array[4];
  buffer[offset++] = uInt8Float64Array[5];
  buffer[offset++] = uInt8Float64Array[6];
  buffer[offset++] = uInt8Float64Array[7];
  return offset;
}

function writeDoubleBackwards(buffer: Buffer|Uint8Array, val: number, offset: number = 0) {
  val = +val;
  checkBounds(buffer, offset, 7);

  float64Array[0] = val;
  buffer[offset++] = uInt8Float64Array[7];
  buffer[offset++] = uInt8Float64Array[6];
  buffer[offset++] = uInt8Float64Array[5];
  buffer[offset++] = uInt8Float64Array[4];
  buffer[offset++] = uInt8Float64Array[3];
  buffer[offset++] = uInt8Float64Array[2];
  buffer[offset++] = uInt8Float64Array[1];
  buffer[offset++] = uInt8Float64Array[0];
  return offset;
}

function readFloatBackwards(buffer: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buffer.length);
  const first = buffer[offset];
  const last = buffer[offset + 3];
  if (first === undefined || last === undefined) {
    boundsError(offset, buffer.length - 4);
  }

  uInt8Float32Array[3] = first;
  uInt8Float32Array[2] = buffer[++offset];
  uInt8Float32Array[1] = buffer[++offset];
  uInt8Float32Array[0] = last;
  return float32Array[0];
}

function readFloatForwards(buffer: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buffer.length);
  const first = buffer[offset];
  const last = buffer[offset + 3];
  if (first === undefined || last === undefined) {
    boundsError(offset, buffer.length - 4);
  }

  uInt8Float32Array[0] = first;
  uInt8Float32Array[1] = buffer[++offset];
  uInt8Float32Array[2] = buffer[++offset];
  uInt8Float32Array[3] = last;
  return float32Array[0];
}

function writeFloatForwards(buffer: Buffer|Uint8Array, val: number, offset: number = 0) {
  val = +val;
  checkBounds(buffer, offset, 3);

  float32Array[0] = val;
  buffer[offset++] = uInt8Float32Array[0];
  buffer[offset++] = uInt8Float32Array[1];
  buffer[offset++] = uInt8Float32Array[2];
  buffer[offset++] = uInt8Float32Array[3];
  return offset;
}

function writeFloatBackwards(buffer: Buffer|Uint8Array, val: number, offset: number = 0) {
  val = +val;
  checkBounds(buffer, offset, 3);

  float32Array[0] = val;
  buffer[offset++] = uInt8Float32Array[3];
  buffer[offset++] = uInt8Float32Array[2];
  buffer[offset++] = uInt8Float32Array[1];
  buffer[offset++] = uInt8Float32Array[0];
  return offset;
}

function readInt24LE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 2];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 3);
  }

  const val = first + buf[++offset] * 2 ** 8 + last * 2 ** 16;
  return val | (val & 2 ** 23) * 0x1fe;
}

function readInt40LE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 4];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 5);
  }

  return (last | (last & 2 ** 7) * 0x1fffffe) * 2 ** 32 +
    first +
    buf[++offset] * 2 ** 8 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 24;
}

function readInt48LE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 5];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 6);
  }

  const val = buf[offset + 4] + last * 2 ** 8;
  return (val | (val & 2 ** 15) * 0x1fffe) * 2 ** 32 +
    first +
    buf[++offset] * 2 ** 8 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 24;
}

function readInt24BE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 2];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 3);
  }

  const val = first * 2 ** 16 + buf[++offset] * 2 ** 8 + last;
  return val | (val & 2 ** 23) * 0x1fe;
}

function readInt48BE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 5];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 6);
  }

  const val = buf[++offset] + first * 2 ** 8;
  return (val | (val & 2 ** 15) * 0x1fffe) * 2 ** 32 +
    buf[++offset] * 2 ** 24 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 8 +
    last;
}

function readInt40BE(buf: Buffer|Uint8Array, offset: number = 0) {
  validateOffset(offset, "offset", 0, buf.length);
  const first = buf[offset];
  const last = buf[offset + 4];
  if (first === undefined || last === undefined) {
    boundsError(offset, buf.length - 5);
  }

  return (first | (first & 2 ** 7) * 0x1fffffe) * 2 ** 32 +
    buf[++offset] * 2 ** 24 +
    buf[++offset] * 2 ** 16 +
    buf[++offset] * 2 ** 8 +
    last;
}

function boundsError(value: number, length: number, type?: string) : never {
  if (Math.floor(value) !== value) {
    throw new ERR_OUT_OF_RANGE(type || "offset", "an integer", value);
  }

  if (length < 0) {
    throw new ERR_BUFFER_OUT_OF_BOUNDS();
  }

  throw new ERR_OUT_OF_RANGE(
    type || "offset",
    `>= ${type ? 1 : 0} and <= ${length}`,
    value,
  );
}

function validateNumber(value: unknown, name: string) {
  if (typeof value !== "number") {
    throw new ERR_INVALID_ARG_TYPE(name, "number", value);
  }
}

function checkInt(
    value: number|bigint,
    min: number|bigint,
    max: number|bigint,
    buf: Buffer,
    offset: number,
    byteLength: number) {
  if (value > max || value < min) {
    const n = typeof min === "bigint" ? "n" : "";
    let range;
    if (byteLength > 3) {
      if (min === 0 || min === 0n) {
        range = `>= 0${n} and < 2${n} ** ${(byteLength + 1) * 8}${n}`;
      } else {
        range = `>= -(2${n} ** ${(byteLength + 1) * 8 - 1}${n}) and ` +
          `< 2${n} ** ${(byteLength + 1) * 8 - 1}${n}`;
      }
    } else {
      range = `>= ${min}${n} and <= ${max}${n}`;
    }
    throw new ERR_OUT_OF_RANGE("value", range, value);
  }
  checkBounds(buf, offset, byteLength);
}

function toInteger(n: number|undefined, defaultVal: number) {
  if (n === undefined) n = 0;
  n = +(n as number);
  if (
    !Number.isNaN(n) &&
    n >= Number.MIN_SAFE_INTEGER &&
    n <= Number.MAX_SAFE_INTEGER
  ) {
    return ((n % 1) === 0 ? n : Math.floor(n));
  }
  return defaultVal;
}

function writeU_Int8(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  if (value > max || value < min) {
    throw new ERR_OUT_OF_RANGE("value", `>= ${min} and <= ${max}`, value);
  }
  if (buf[offset] === undefined) {
    boundsError(offset, buf.length - 1);
  }

  buf[offset] = value;
  return offset + 1;
}

function writeU_Int16BE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 1);

  buf[offset++] = value >>> 8;
  buf[offset++] = value;
  return offset;
}

function _writeUInt32LE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 3);

  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  return offset;
}

function writeU_Int16LE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 1);

  buf[offset++] = value;
  buf[offset++] = value >>> 8;
  return offset;
}

function _writeUInt32BE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 3);

  buf[offset + 3] = value;
  value = value >>> 8;
  buf[offset + 2] = value;
  value = value >>> 8;
  buf[offset + 1] = value;
  value = value >>> 8;
  buf[offset] = value;
  return offset + 4;
}

function writeU_Int48BE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 5);

  const newVal = Math.floor(value * 2 ** -32);
  buf[offset++] = newVal >>> 8;
  buf[offset++] = newVal;
  buf[offset + 3] = value;
  value = value >>> 8;
  buf[offset + 2] = value;
  value = value >>> 8;
  buf[offset + 1] = value;
  value = value >>> 8;
  buf[offset] = value;
  return offset + 4;
}

function writeU_Int40BE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 4);

  buf[offset++] = Math.floor(value * 2 ** -32);
  buf[offset + 3] = value;
  value = value >>> 8;
  buf[offset + 2] = value;
  value = value >>> 8;
  buf[offset + 1] = value;
  value = value >>> 8;
  buf[offset] = value;
  return offset + 4;
}

function writeU_Int32BE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 3);

  buf[offset + 3] = value;
  value = value >>> 8;
  buf[offset + 2] = value;
  value = value >>> 8;
  buf[offset + 1] = value;
  value = value >>> 8;
  buf[offset] = value;
  return offset + 4;
}

function writeU_Int24BE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 2);

  buf[offset + 2] = value;
  value = value >>> 8;
  buf[offset + 1] = value;
  value = value >>> 8;
  buf[offset] = value;
  return offset + 3;
}

function validateOffset(
  value: number,
  name: string,
  min: number = 0,
  max: number = Number.MAX_SAFE_INTEGER,
) {
  if (typeof value !== "number") {
    throw new ERR_INVALID_ARG_TYPE(name, "number", value);
  }
  if (!Number.isInteger(value)) {
    throw new ERR_OUT_OF_RANGE(name, "an integer", value);
  }
  if (value < min || value > max) {
    throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
  }
}

function writeU_Int48LE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 5);

  const newVal = Math.floor(value * 2 ** -32);
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  buf[offset++] = newVal;
  buf[offset++] = newVal >>> 8;
  return offset;
}

function writeU_Int40LE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 4);

  const newVal = value;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  buf[offset++] = Math.floor(newVal * 2 ** -32);
  return offset;
}

function writeU_Int32LE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 3);

  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  return offset;
}

function writeU_Int24LE(
    buf: Buffer,
    value: number,
    offset: number,
    min: number,
    max: number) {
  value = +value;
  validateOffset(offset, "offset", 0, buf.length);
  checkInt(value, min, max, buf, offset, 2);

  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  value = value >>> 8;
  buf[offset++] = value;
  return offset;
}

export default {
  Buffer,
  constants,
  kMaxLength,
  kStringMaxLength,
  SlowBuffer,
};
