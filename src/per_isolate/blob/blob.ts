'use strict';

// Blob and File (W3C File API), under the TS_BLOB autogate. They replace the
// C++ classes in api/blob.h, whose observable behavior they keep, down to
// the bytes they write: src/tests/blob/ runs one suite against both
// implementations, as api/blob-bootstrap-test.c++ does for the C++ code
// that makes and reads a Blob through api/blob-bridge.h.
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
import type { TypeCheckHelpers } from '../type-checks';

const {
  ArrayBufferPrototypeByteLengthGet,
  ArrayIsArray,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  Date,
  NumberIsFinite,
  NumberIsNaN,
  ObjectCreate,
  ObjectDefineProperty,
  ObjectGetOwnPropertyDescriptor,
  ObjectSetPrototypeOf,
  PromiseResolve,
  RangeError,
  RegExpPrototypeExec,
  StringPrototypeCharCodeAt,
  StringPrototypeIndexOf,
  StringPrototypeIsWellFormed,
  StringPrototypeSlice,
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
  encodeWtf8,
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
const { isObject } = require('type-checks') as TypeCheckHelpers;

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

// Made on first use, since every isolate runs this module and most never
// read a Blob.
let encoder: TextEncoder | undefined;
let decoder: TextDecoder | undefined;

function getEncoder(): TextEncoder {
  return (encoder ??= new TextEncoder());
}

// Decodes as the C++ Blob's text() does: replacing invalid sequences, and
// keeping a byte order mark. Made after user code may have run, so the
// options inherit nothing (such as a `fatal` on Object.prototype).
function getDecoder(): TextDecoder {
  return (decoder ??= new TextDecoder('utf-8', {
    __proto__: null,
    ignoreBOM: true,
  } as TextDecoderOptions));
}

// Any code unit outside printable ASCII.
const kNonPrintableAscii = /[^\x20-\x7e]/;

// An instance's state, under the private properties above.
type Slots = Record<symbol, unknown>;

// A part of a Blob, once converted from what the constructor was given: a
// Blob, a BufferSource or a string. A BufferSource's bytes are taken, and a
// string encoded, only once every part is converted, since converting a
// later part can resize an earlier one's buffer, as they are in C++.
type Part = object | ArrayBuffer | ArrayBufferView | string;

// ---------------------------------------------------------------------------
// Conversions, as JSG converts the C++ constructors' and slice()'s arguments.

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

// A slice() start or end, already a number, as an offset into a Blob of
// `size` bytes, as the C++ converts a WebIDL [Clamp] long long: NaN as 0,
// rounded to the nearest integer with ties to even, a negative value counted
// back from the end, and clamped to the Blob.
function relativeOffset(value: number, size: number): number {
  if (NumberIsNaN(value)) return 0;
  if (!NumberIsFinite(value)) return value < 0 ? 0 : size;
  const truncated = value - (value % 1);
  const floor = truncated > value ? truncated - 1 : truncated;
  const fraction = value - floor;
  let offset =
    fraction < 0.5 || (fraction === 0.5 && floor % 2 === 0) ? floor : floor + 1;
  if (offset < 0) offset += size;
  return offset < 0 ? 0 : offset > size ? size : offset;
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

// The `endings` option, read and converted to a string as JSG reads the
// field; requireNativeEndings() checks it once every field is read.
function convertEndings(options: object | undefined): string | undefined {
  if (options === undefined) return undefined;
  const endings: unknown = (options as { endings?: unknown }).endings;
  return endings === undefined ? undefined : toString(endings);
}

// Whether `endings` asks for string parts' line endings to be converted to
// the native one.
function requireNativeEndings(endings: string | undefined): boolean {
  if (endings === undefined || endings === 'transparent') return false;
  if (endings === 'native') return true;
  throw new TypeError(
    `The provided value '${fromWtf8(endings)}' is not a valid enum value of ` +
      'type EndingType.'
  );
}

// `string` with every CRLF and lone CR as LF, which is the native line
// ending on every platform, as in the C++.
function convertLineEndings(string: string): string {
  let cr = StringPrototypeIndexOf(string, '\r') as number;
  if (cr === -1) return string;
  let result = '';
  let start = 0;
  while (cr !== -1) {
    result += `${StringPrototypeSlice(string, start, cr)}\n`;
    start =
      StringPrototypeCharCodeAt(string, cr + 1) === 0x0a ? cr + 2 : cr + 1;
    cr = StringPrototypeIndexOf(string, '\r', start) as number;
  }
  return result + StringPrototypeSlice(string, start);
}

// The parts' bytes, concatenated: a single Blob's shared rather than copied.
// Each part is checked in turn, as C++ checks them: a resizable buffer
// (under no_resizable_array_buffer_in_blob), then the running size against
// the limit.
function combine(parts: Part[], nativeEndings: boolean): Uint8Array {
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
      bytes = encodeString(nativeEndings ? convertLineEndings(part) : part);
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

// `string` as WTF-8.
function encodeString(string: string): Uint8Array {
  // TextEncoder is faster, and writes a well-formed string the same way.
  if (StringPrototypeIsWellFormed(string)) {
    return TextEncoderEncode(getEncoder(), string);
  }
  return encodeWtf8(string);
}

// `string` as JavaScript sees a string C++ keeps as WTF-8 bytes: decoded,
// so that each lone surrogate's three bytes are three U+FFFDs.
function fromWtf8(string: string): string {
  if (StringPrototypeIsWellFormed(string)) return string;
  return TextDecoderDecode(getDecoder(), encodeString(string)) as string;
}

// ---------------------------------------------------------------------------
// Instances

// The descriptor of an attribute under the legacy layout, reused for every
// one, so that initInstance() makes no descriptor objects. Its prototype is
// null, so that defining with it reads nothing user code can patch.
class AttributeDescriptor {
  declare value: unknown;
  declare writable: boolean;
  declare enumerable: boolean;
  declare configurable: boolean;
  constructor() {
    this.value = undefined;
    this.writable = false;
    this.enumerable = true;
    this.configurable = true;
  }
}
ObjectSetPrototypeOf(AttributeDescriptor.prototype, null);
const kAttribute = new AttributeDescriptor();

// Defines nothing user code can observe while it runs: `self` is an ordinary
// object this module made.
function defineAttribute(self: object, name: string, value: unknown): void {
  kAttribute.value = value;
  ObjectDefineProperty(self, name, kAttribute as PropertyDescriptor);
  kAttribute.value = undefined;
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
      defineAttribute(self, 'lastModified', lastModified);
      defineAttribute(self, 'name', fromWtf8(name));
    }
    defineAttribute(self, 'type', type);
    defineAttribute(self, 'size', TypedArrayPrototypeGetByteLength(bytes));
  }
  if (name === undefined) {
    markStandIn(self, kBlobSerializationTag);
  } else {
    markUncloneable(self);
  }
}

// The constructors are functions rather than classes so that they have the
// shape JSG gives the C++ ones: a function's `prototype` is writable, as a
// JSG constructor's is unless immutable_api_prototypes makes it read-only,
// and a function can throw JSG's error when called without `new`. Neither
// inherits from anything (File's [[Prototype]] is Function.prototype, as in
// C++), so neither calls anything user code can replace while constructing.

const kCallWithNew =
  ": Please use the 'new' operator, this object constructor cannot be " +
  'called as a function.';

function Blob(this: object, blobParts?: unknown, options?: unknown): void {
  if (new.target === undefined) {
    throw new TypeError(kConstructBlob + kCallWithNew);
  }
  const parts = convertParts(blobParts, kConstructBlob);
  const bag = requireOptions(options, kConstructBlob, 2);
  const type = convertType(bag);
  const nativeEndings = requireNativeEndings(convertEndings(bag));
  initInstance(this, combine(parts, nativeEndings), type);
}

function File(
  this: object,
  fileBits?: unknown,
  fileName?: unknown,
  options?: unknown
): void {
  if (new.target === undefined) {
    throw new TypeError(kConstructFile + kCallWithNew);
  }
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
  const nativeEndings = requireNativeEndings(convertEndings(bag));
  initInstance(this, combine(parts, nativeEndings), type, name, lastModified);
}

// The prototypes' members, in JSG's order. Methods and accessors defined in
// an object literal are enumerable, as JSG's are, and like a class's methods
// are not constructors.
const kBlobMembers = {
  get size(): number {
    return TypedArrayPrototypeGetByteLength(requireBytes(this)) as number;
  },

  get type(): string {
    requireBytes(this);
    return (this as unknown as Slots)[kType] as string;
  },

  slice(start?: unknown, end?: unknown, contentType?: unknown): object {
    const bytes = requireBytes(this);
    // ToNumber, which throws for a Symbol or a BigInt.
    const startArg = start === undefined ? undefined : +(start as number);
    const endArg = end === undefined ? undefined : +(end as number);
    const type =
      contentType === undefined ? '' : normalizeType(toString(contentType));
    const size = TypedArrayPrototypeGetByteLength(bytes) as number;
    const from = startArg === undefined ? 0 : relativeOffset(startArg, size);
    let to = endArg === undefined ? size : relativeOffset(endArg, size);
    if (to < from) to = from;
    const blob = ObjectCreate(kBlobPrototype) as object;
    initInstance(blob, subrange(bytes, from, to), type);
    return blob;
  },

  arrayBuffer(): Promise<ArrayBuffer> {
    const bytes = requireBytes(this);
    recordBlobUse(0);
    return PromiseResolve(
      TypedArrayPrototypeGetBuffer(copy(bytes)) as ArrayBuffer
    );
  },

  bytes(): Promise<Uint8Array> {
    const bytes = requireBytes(this);
    recordBlobUse(0);
    return PromiseResolve(copy(bytes));
  },

  text(): Promise<string> {
    const bytes = requireBytes(this);
    recordBlobUse(1);
    return PromiseResolve(TextDecoderDecode(getDecoder(), bytes));
  },

  stream(): ReadableStream<Uint8Array> {
    const bytes = requireBytes(this);
    recordBlobUse(2);
    return blobStream(bytes);
  },
};

const kFileMembers = {
  get name(): string {
    return fromWtf8(requireFile(this)[kName] as string);
  },

  get lastModified(): number {
    return requireFile(this)[kLastModified] as number;
  },
};

// ---------------------------------------------------------------------------
// The rest gives the constructors the shape JSG gives the C++ ones.

// Defines `members` on `ctor.prototype`, each method named and of length 0,
// as JSG's are. Under the legacy layout, the accessors are left out: the
// attributes are the instances' own. The prototype keeps the `constructor`
// the function was made with, which is already as JSG's is (writable,
// configurable, not enumerable), though first rather than after the members.
function shapePrototype(ctor: object, members: object, names: string[]): void {
  const proto = (ctor as { prototype: object }).prototype;
  for (let i = 0; i < names.length; i++) {
    const name = names[i] as string;
    const descriptor = ObjectGetOwnPropertyDescriptor(
      members,
      name
    ) as PropertyDescriptor;
    if (descriptor.get !== undefined) {
      if (kInstanceProperties) continue;
    } else {
      setNameAndLength(descriptor.value, name, 0);
    }
    ObjectDefineProperty(proto, name, descriptor);
  }
  if (compatFlags['immutable_api_prototypes'] === true) {
    ObjectDefineProperty(ctor, 'prototype', {
      __proto__: null,
      writable: false,
    } as PropertyDescriptor);
  }
}

// Captured because Blob.prototype stays writable unless immutable_api_prototypes
// is set, and slice() must not depend on what user code assigns there.
const kBlobPrototype: object = Blob.prototype;
ObjectSetPrototypeOf(File.prototype, kBlobPrototype);
shapePrototype(Blob, kBlobMembers, [
  'size',
  'type',
  'slice',
  'arrayBuffer',
  'bytes',
  'text',
  'stream',
]);
shapePrototype(File, kFileMembers, ['name', 'lastModified']);
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
