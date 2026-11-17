'use strict';

// Blob and File (W3C File API), under the TS_BLOB autogate. They replace the
// C++ classes in api/blob.h, whose observable behavior they keep, but where
// src/tests/blob/AGENTS.md lists a difference; its suite runs against both
// implementations, as does the C++ code that makes and reads a Blob through
// api/blob-bridge.h.
//
// An instance keeps its state under V8-private properties rather than
// #private fields, because the C++ bridge makes instances without running
// JavaScript (in V8's deserializer, for one) and reads them the same way:
// the bytes, a Uint8Array that is also the brand, and the type, and for a
// File the name and lastModified. The bridge's newTsInstance() makes an
// instance as initInstance() does: keep the two in step.
//
// The bytes are never written once an instance has them, so a Blob made
// from a single Blob, a slice, and a File made from a Blob share their
// parent's. The reading methods hand out copies.
//
// A Blob serializes as the C++ Blob does (markStandIn()), so either
// implementation, and an RPC peer running either, reads what the other
// writes. A File, as in C++, does not serialize.

import type { InternalArrayHelpers } from '../url/internal-array';
import type { RuntimeClassHelpers } from '../runtime-class';
import type { SerializationHelpers } from '../serialization';

const {
  ArrayBufferPrototypeByteLengthGet,
  ArrayIsArray,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  Date,
  Error,
  NumberIsFinite,
  NumberIsNaN,
  ObjectCreate,
  ObjectDefineProperties,
  ObjectDefineProperty,
  ObjectGetOwnPropertyDescriptor,
  ObjectSetPrototypeOf,
  PromiseResolve,
  RangeError,
  RegExpPrototypeExec,
  StringPrototypeCharCodeAt,
  StringPrototypeIsWellFormed,
  StringPrototypeToLowerCase,
  TextDecoder,
  TextDecoderDecode,
  TextEncoder,
  TextEncoderEncode,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeGetSymbolToStringTag,
  TypedArrayPrototypeSet,
  TypeError,
  Uint8Array,
} = primordials;

const {
  blobSizeLimit,
  blobStream,
  getPrivateSymbol,
  isArrayBuffer,
  isArrayBufferView,
  isProxy,
  isResizableBufferSource,
  recordBlobUse,
} = utils;

const { markAsRuntimeClass, setNameAndLength } =
  require('runtime-class') as RuntimeClassHelpers;
const { markStandIn, markUncloneable } =
  require('serialization') as SerializationHelpers;
const { internalArray } = require('url/internal-array') as InternalArrayHelpers;

// Shared with api/blob-bridge.c++.
const kBytes = getPrivateSymbol('workerd:Blob:bytes');
const kType = getPrivateSymbol('workerd:Blob:type');
const kName = getPrivateSymbol('workerd:File:name');
const kLastModified = getPrivateSymbol('workerd:File:lastModified');

// rpc::SerializationTag::BLOB, the C++ Blob's tag.
const kBlobSerializationTag = 15;

// Under the legacy layout (no workers_api_getters_setters_on_prototype), the
// attributes are own read-only data properties of each instance rather than
// accessors of the prototype.
const kInstanceProperties =
  compatFlags['workers_api_getters_setters_on_prototype'] !== true;

const kRejectResizable =
  compatFlags['no_resizable_array_buffer_in_blob'] === true;

const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const kConstructBlob = "Failed to construct 'Blob'";
const kConstructFile = "Failed to construct 'File'";

const { now: DateNow } = Date;

const encoder = new TextEncoder();
// As the C++ Blob's text() decodes: replacing invalid sequences, and keeping
// a byte order mark.
const decoder = new TextDecoder('utf-8', { ignoreBOM: true });

// Any code unit outside printable ASCII (DEL counts as printable here, as in
// the C++).
const kNonPrintableAscii = /[^\x20-\x7f]/;

// An instance's state, under the private properties above.
type Slots = Record<symbol, unknown>;

// A part of a Blob, once converted from what the constructor was given: a
// Blob, a BufferSource or a string. A BufferSource's bytes are taken, and a
// string encoded, only once every part is converted, since converting a
// later part can resize an earlier one's buffer, as they are in C++.
type Part = object | ArrayBuffer | ArrayBufferView | string;

// ---------------------------------------------------------------------------
// Conversions, as JSG converts the C++ constructors' and slice()'s arguments.

function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

// The bytes of a Blob of this implementation, or undefined.
function bytesOf(value: unknown): Uint8Array | undefined {
  return isObject(value)
    ? ((value as Slots)[kBytes] as Uint8Array | undefined)
    : undefined;
}

function isBlob(value: unknown): boolean {
  return bytesOf(value) !== undefined;
}

// The bytes of `value`, which must be a Blob: a method's brand check.
function requireBytes(value: unknown): Uint8Array {
  const bytes = bytesOf(value);
  if (bytes === undefined) throw new TypeError(kIllegalInvocation);
  return bytes;
}

function requireFile(value: unknown): Slots {
  if (!isObject(value) || (value as Slots)[kName] === undefined) {
    throw new TypeError(kIllegalInvocation);
  }
  return value as Slots;
}

// ToString, which throws for a Symbol.
function toString(value: unknown): string {
  return `${value as string}`;
}

// JSG's int: an int32 as it is, anything else through ToNumber, non-finite
// numbers as 0, and the rest truncated, if within the int32 range.
function toInt(value: unknown): number {
  if (typeof value === 'number' && (value | 0) === value) return value;
  const number = +(value as number);
  if (!NumberIsFinite(number)) return 0;
  if (number >= 2147483648 || number <= -2147483649) {
    throw new TypeError(
      'Value out of range. Must be between -2147483648 and 2147483647 (inclusive).'
    );
  }
  return number | 0;
}

// The type as the C++ normalizes it: discarded if any code unit is outside
// printable ASCII, otherwise lower-cased.
function normalizeType(type: string): string {
  if (RegExpPrototypeExec(kNonPrintableAscii, type) !== null) return '';
  return StringPrototypeToLowerCase(type);
}

// The bytes a BufferSource views, as a Uint8Array over them.
function viewBytes(source: ArrayBuffer | ArrayBufferView): Uint8Array {
  // A view is never made over a detached buffer, which has no bytes:
  // constructing one throws.
  if (isArrayBuffer(source)) {
    const length = ArrayBufferPrototypeByteLengthGet(source) as number;
    return length === 0 ? new Uint8Array(0) : new Uint8Array(source, 0, length);
  }
  let buffer: ArrayBufferLike;
  let byteOffset = 0;
  let byteLength = 0;
  if (TypedArrayPrototypeGetSymbolToStringTag(source) !== undefined) {
    buffer = TypedArrayPrototypeGetBuffer(source) as ArrayBufferLike;
    byteOffset = TypedArrayPrototypeGetByteOffset(source) as number;
    byteLength = TypedArrayPrototypeGetByteLength(source) as number;
  } else {
    buffer = DataViewPrototypeGetBuffer(source as DataView) as ArrayBufferLike;
    // A DataView's getters throw once its buffer is detached or has shrunk
    // out from under it; it then has no bytes, as it has none in C++.
    try {
      byteOffset = DataViewPrototypeGetByteOffset(source as DataView) as number;
      byteLength = DataViewPrototypeGetByteLength(source as DataView) as number;
    } catch {
      return new Uint8Array(0);
    }
  }
  if (byteLength === 0) return new Uint8Array(0);
  return new Uint8Array(buffer, byteOffset, byteLength);
}

// The constructor's first argument, converted as JSG converts an optional
// kj::Array of the C++ Blob's parts: a real Array, each element read in turn
// and kept if it is a Blob or a BufferSource (a SharedArrayBuffer is not
// one), otherwise converted to a string, kept as its UTF-8.
function convertParts(value: unknown, what: string): Part[] {
  if (value === undefined) return [];
  // JSG's IsArray() check, which, unlike Array.isArray(), rejects a proxy.
  if (!ArrayIsArray(value) || isProxy(value)) {
    throw new TypeError(
      `${what}: constructor parameter 1 is not of type 'Array'.`
    );
  }
  const array = value as unknown[];
  const length = array.length;
  const parts = internalArray<Part>();
  for (let i = 0; i < length; i++) {
    const element: unknown = array[i];
    parts[parts.length] =
      isBlob(element) || isArrayBuffer(element) || isArrayBufferView(element)
        ? (element as Part)
        : toString(element);
  }
  return parts;
}

// The options bag, as JSG converts a JSG_STRUCT: undefined or null for none,
// otherwise an object, whose fields are read in their declared order.
function requireOptions(
  value: unknown,
  what: string,
  index: number
): object | undefined {
  if (value === undefined || value === null) return undefined;
  if (!isObject(value)) {
    throw new TypeError(
      `${what}: constructor parameter ${index} is not of type 'Options'.`
    );
  }
  return value;
}

function convertType(options: object | undefined): string {
  if (options === undefined) return '';
  const type: unknown = (options as { type?: unknown }).type;
  return type === undefined ? '' : normalizeType(toString(type));
}

function checkEndings(options: object | undefined): void {
  if (
    options !== undefined &&
    (options as { endings?: unknown }).endings !== undefined
  ) {
    throw new Error("The 'endings' field on 'Options' is not implemented.");
  }
}

// The parts' bytes, concatenated: a single Blob's shared rather than copied.
// Each part is checked in turn, as C++ checks them: a resizable buffer
// (under no_resizable_array_buffer_in_blob), then the running size against
// the limit.
function combine(parts: Part[]): Uint8Array {
  if (parts.length === 1) {
    const only = bytesOf(parts[0]);
    if (only !== undefined) return only;
  }
  const limit = blobSizeLimit();
  const views = internalArray<Uint8Array>();
  let size = 0;
  for (let i = 0; i < parts.length; i++) {
    const part = parts[i] as Part;
    let bytes: Uint8Array;
    if (typeof part === 'string') {
      bytes = encodeString(part);
    } else if (isArrayBuffer(part) || isArrayBufferView(part)) {
      if (kRejectResizable && isResizableBufferSource(part)) {
        throw new TypeError(
          'Cannot create a Blob with a resizable ArrayBuffer'
        );
      }
      bytes = viewBytes(part);
    } else {
      bytes = bytesOf(part) as Uint8Array;
    }
    views[views.length] = bytes;
    const partSize = TypedArrayPrototypeGetByteLength(bytes) as number;
    if (partSize === 0) continue;
    if (size + partSize > limit) {
      throw new RangeError(
        `Blob size ${size + partSize} exceeds limit ${limit}`
      );
    }
    size += partSize;
  }
  const result = new Uint8Array(size);
  if (size === 0) return result;
  let offset = 0;
  for (let i = 0; i < views.length; i++) {
    const bytes = views[i] as Uint8Array;
    TypedArrayPrototypeSet(result, bytes, offset);
    offset += TypedArrayPrototypeGetByteLength(bytes) as number;
  }
  return result;
}

// A Uint8Array over bytes `from` to `to` of `bytes`, sharing them. Made
// directly rather than with subarray(), which looks up a constructor through
// the prototype chain, where user code may have replaced it.
function subrange(bytes: Uint8Array, from: number, to: number): Uint8Array {
  if (to === from) return new Uint8Array(0);
  return new Uint8Array(
    TypedArrayPrototypeGetBuffer(bytes) as ArrayBufferLike,
    (TypedArrayPrototypeGetByteOffset(bytes) as number) + from,
    to - from
  );
}

// A copy of `bytes`, made directly rather than with slice() (see subrange()).
function copy(bytes: Uint8Array): Uint8Array {
  const result = new Uint8Array(
    TypedArrayPrototypeGetByteLength(bytes) as number
  );
  TypedArrayPrototypeSet(result, bytes);
  return result;
}

// ---------------------------------------------------------------------------
// Strings, which become bytes as JSG makes a kj::String of them: as WTF-8,
// so a lone surrogate is written as its own three bytes, as no well-formed
// UTF-8 sequence is.

function isLeadSurrogate(unit: number): boolean {
  return unit >= 0xd800 && unit <= 0xdbff;
}

function isTrailSurrogate(unit: number): boolean {
  return unit >= 0xdc00 && unit <= 0xdfff;
}

// `string` as WTF-8.
function encodeString(string: string): Uint8Array {
  if (StringPrototypeIsWellFormed(string)) {
    return TextEncoderEncode(encoder, string);
  }
  const length = string.length;
  let size = 0;
  for (let i = 0; i < length; i++) {
    const unit = StringPrototypeCharCodeAt(string, i) as number;
    if (unit < 0x80) {
      size += 1;
    } else if (unit < 0x800) {
      size += 2;
    } else if (
      isLeadSurrogate(unit) &&
      i + 1 < length &&
      isTrailSurrogate(StringPrototypeCharCodeAt(string, i + 1) as number)
    ) {
      size += 4;
      i++;
    } else {
      size += 3;
    }
  }
  const bytes = new Uint8Array(size);
  let at = 0;
  for (let i = 0; i < length; i++) {
    let unit = StringPrototypeCharCodeAt(string, i) as number;
    if (unit < 0x80) {
      bytes[at++] = unit;
    } else if (unit < 0x800) {
      bytes[at++] = 0xc0 | (unit >> 6);
      bytes[at++] = 0x80 | (unit & 0x3f);
    } else if (
      isLeadSurrogate(unit) &&
      i + 1 < length &&
      isTrailSurrogate(StringPrototypeCharCodeAt(string, i + 1) as number)
    ) {
      const trail = StringPrototypeCharCodeAt(string, ++i) as number;
      unit = 0x10000 + ((unit - 0xd800) << 10) + (trail - 0xdc00);
      bytes[at++] = 0xf0 | (unit >> 18);
      bytes[at++] = 0x80 | ((unit >> 12) & 0x3f);
      bytes[at++] = 0x80 | ((unit >> 6) & 0x3f);
      bytes[at++] = 0x80 | (unit & 0x3f);
    } else {
      bytes[at++] = 0xe0 | (unit >> 12);
      bytes[at++] = 0x80 | ((unit >> 6) & 0x3f);
      bytes[at++] = 0x80 | (unit & 0x3f);
    }
  }
  return bytes;
}

// `string` as JavaScript sees a string C++ keeps as WTF-8 bytes: decoded,
// so that each lone surrogate's three bytes are three U+FFFDs.
function fromWtf8(string: string): string {
  if (StringPrototypeIsWellFormed(string)) return string;
  return TextDecoderDecode(decoder, encodeString(string)) as string;
}

// ---------------------------------------------------------------------------
// Instances

function instanceAttribute(value: unknown): PropertyDescriptor {
  return {
    __proto__: null,
    value,
    writable: false,
    enumerable: true,
    configurable: true,
  } as PropertyDescriptor;
}

// Gives `self` its state, its own properties under the legacy layout, in
// JSG's order (a File's own before a Blob's), and its serialization mark.
function initInstance(
  self: object,
  bytes: Uint8Array,
  type: string,
  name?: string,
  lastModified?: number
): void {
  const slots = self as Slots;
  slots[kBytes] = bytes;
  slots[kType] = type;
  if (name !== undefined) {
    slots[kName] = name;
    slots[kLastModified] = lastModified as number;
  }
  if (kInstanceProperties) {
    if (name !== undefined) {
      ObjectDefineProperties(self, {
        __proto__: null,
        lastModified: instanceAttribute(lastModified),
        name: instanceAttribute(fromWtf8(name)),
      });
    }
    ObjectDefineProperties(self, {
      __proto__: null,
      type: instanceAttribute(type),
      size: instanceAttribute(TypedArrayPrototypeGetByteLength(bytes)),
    });
  }
  if (name === undefined) {
    markStandIn(self, kBlobSerializationTag);
  } else {
    markUncloneable(self);
  }
}

class Blob {
  constructor(blobParts?: unknown, options?: unknown) {
    const parts = convertParts(blobParts, kConstructBlob);
    const bag = requireOptions(options, kConstructBlob, 2);
    const type = convertType(bag);
    checkEndings(bag);
    initInstance(this, combine(parts), type);
  }

  get size(): number {
    return TypedArrayPrototypeGetByteLength(requireBytes(this)) as number;
  }

  get type(): string {
    requireBytes(this);
    return (this as unknown as Slots)[kType] as string;
  }

  slice(start?: unknown, end?: unknown, contentType?: unknown): Blob {
    const bytes = requireBytes(this);
    const startArg = start === undefined ? undefined : toInt(start);
    const endArg = end === undefined ? undefined : toInt(end);
    const type =
      contentType === undefined ? '' : normalizeType(toString(contentType));
    const size = TypedArrayPrototypeGetByteLength(bytes) as number;
    let from = startArg ?? 0;
    let to = endArg ?? size;
    if (from < 0) from += size;
    if (to < 0) to += size;
    from = from < 0 ? 0 : from > size ? size : from;
    to = to < from ? from : to > size ? size : to;
    const blob = ObjectCreate(Blob.prototype) as Blob;
    initInstance(blob, subrange(bytes, from, to), type);
    return blob;
  }

  arrayBuffer(): Promise<ArrayBuffer> {
    const bytes = requireBytes(this);
    recordBlobUse(0);
    return PromiseResolve(
      TypedArrayPrototypeGetBuffer(copy(bytes)) as ArrayBuffer
    );
  }

  bytes(): Promise<Uint8Array> {
    const bytes = requireBytes(this);
    recordBlobUse(0);
    return PromiseResolve(copy(bytes));
  }

  text(): Promise<string> {
    const bytes = requireBytes(this);
    recordBlobUse(1);
    return PromiseResolve(TextDecoderDecode(decoder, bytes));
  }

  stream(): ReadableStream<Uint8Array> {
    const bytes = requireBytes(this);
    recordBlobUse(2);
    return blobStream(bytes);
  }
}

// Not `extends Blob`: a derived constructor's super() calls whatever File's
// [[Prototype]] is then, which user code can replace. File initializes its
// own instance, and inherits from Blob through the prototypes set below.
class File {
  constructor(fileBits?: unknown, fileName?: unknown, options?: unknown) {
    const parts = convertParts(fileBits, kConstructFile);
    if (fileName === undefined) {
      throw new TypeError(
        `${kConstructFile}: constructor parameter 2 is not of type 'string'.`
      );
    }
    const name = toString(fileName);
    const bag = requireOptions(options, kConstructFile, 3);
    const type = convertType(bag);
    let lastModified: number;
    const lastModifiedArg: unknown =
      bag === undefined
        ? undefined
        : (bag as { lastModified?: unknown }).lastModified;
    if (lastModifiedArg === undefined) {
      lastModified = DateNow();
    } else {
      lastModified = +(lastModifiedArg as number);
      if (NumberIsNaN(lastModified)) lastModified = 0;
    }
    checkEndings(bag);
    initInstance(this, combine(parts), type, name, lastModified);
  }

  get name(): string {
    return fromWtf8(requireFile(this)[kName] as string);
  }

  get lastModified(): number {
    return requireFile(this)[kLastModified] as number;
  }
}

// ---------------------------------------------------------------------------
// The rest gives the classes the shape JSG gives the C++ ones.

// Moves `constructor` after the members, where JSG defines it, and makes
// the members enumerable, as JSG's are. Under the legacy layout, the
// accessors are left out: the attributes are the instances' own.
function shapePrototype(proto: object, accessors: readonly string[]): void {
  const ctor = ObjectGetOwnPropertyDescriptor(
    proto,
    'constructor'
  ) as PropertyDescriptor;
  delete (proto as { constructor?: unknown }).constructor;
  for (let i = 0; i < accessors.length; i++) {
    const name = accessors[i] as string;
    if (kInstanceProperties) {
      delete (proto as Record<string, unknown>)[name];
    } else {
      ObjectDefineProperty(proto, name, {
        __proto__: null,
        enumerable: true,
      } as PropertyDescriptor);
    }
  }
  ObjectDefineProperty(proto, 'constructor', {
    __proto__: null,
    value: ctor.value,
    writable: true,
    enumerable: false,
    configurable: true,
  } as PropertyDescriptor);
}

const kBlobMethods = ['slice', 'arrayBuffer', 'bytes', 'text', 'stream'];
for (let i = 0; i < kBlobMethods.length; i++) {
  const name = kBlobMethods[i] as string;
  ObjectDefineProperty(Blob.prototype, name, {
    __proto__: null,
    enumerable: true,
  } as PropertyDescriptor);
  setNameAndLength(
    (ObjectGetOwnPropertyDescriptor(Blob.prototype, name) as PropertyDescriptor)
      .value,
    name,
    0
  );
}
ObjectSetPrototypeOf(File.prototype, Blob.prototype);
ObjectSetPrototypeOf(File, Blob);
shapePrototype(Blob.prototype, ['size', 'type']);
shapePrototype(File.prototype, ['name', 'lastModified']);
setNameAndLength(Blob, 'Blob', 0);
setNameAndLength(File, 'File', 0);
markAsRuntimeClass(Blob.prototype, 'Blob');
markAsRuntimeClass(File.prototype, 'File');

// For the C++ bridge, through blob/cpp_exports.
const cppExports = {
  BlobPrototype: Blob.prototype,
  FilePrototype: File.prototype,
  instanceProperties: kInstanceProperties,
};

export type BlobExports = {
  Blob: typeof Blob;
  File: typeof File;
  isBlob: typeof isBlob;
  cppExports: typeof cppExports;
};

module.exports = {
  Blob,
  File,
  isBlob,
  cppExports,
};
