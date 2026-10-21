'use strict';

// The byte extent of an ArrayBufferView or any other BufferSource, read
// through captured getters, and copies of such extents, for code that
// consumes caller-supplied bytes (the internal transform pairs' write paths,
// the native conduit's enqueue, the digest stream, byte-counting strategies,
// body collection).
//
// A view whose buffer has been detached, or that a resizable buffer has
// shrunk out from under, has no bytes: the typed-array getters report
// byteOffset and byteLength 0 for it, but the DataView getters throw a
// TypeError. These helpers give a DataView the typed arrays' answer, so
// such a view reads as empty whichever kind it is — as it does in the C++
// implementation, which treats it as zero-length. A detached ArrayBuffer
// is empty too: its byteLength getter reports 0. Leaf module: requires
// nothing.

const {
  ArrayBufferPrototypeByteLengthGet,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeGetSymbolToStringTag,
  TypedArrayPrototypeSet,
  Uint8Array,
} = primordials;

const { isArrayBuffer, isArrayBufferView, isSharedArrayBuffer } = utils;

interface ViewExtent {
  buffer: ArrayBufferLike;
  byteOffset: number;
  byteLength: number;
}

// PRECONDITION: isArrayBufferView(view). A view without a [[TypedArrayName]]
// is a DataView, whose getters brand-check the receiver; they throw only for
// a detached or out-of-bounds view.
function viewByteLength(view: ArrayBufferView): number {
  if (TypedArrayPrototypeGetSymbolToStringTag(view) !== undefined) {
    return TypedArrayPrototypeGetByteLength(view) as number;
  }
  try {
    return DataViewPrototypeGetByteLength(view as DataView) as number;
  } catch {
    return 0;
  }
}

// PRECONDITION: isArrayBufferView(view). The buffer getters never throw.
function viewByteExtent(view: ArrayBufferView): ViewExtent {
  if (TypedArrayPrototypeGetSymbolToStringTag(view) !== undefined) {
    return {
      __proto__: null,
      buffer: TypedArrayPrototypeGetBuffer(view),
      byteOffset: TypedArrayPrototypeGetByteOffset(view),
      byteLength: TypedArrayPrototypeGetByteLength(view),
    } as ViewExtent;
  }
  const dataView = view as DataView;
  const buffer = DataViewPrototypeGetBuffer(dataView) as ArrayBufferLike;
  let byteOffset = 0;
  let byteLength = 0;
  try {
    byteOffset = DataViewPrototypeGetByteOffset(dataView) as number;
    byteLength = DataViewPrototypeGetByteLength(dataView) as number;
  } catch {
    byteOffset = 0;
    byteLength = 0;
  }
  return { __proto__: null, buffer, byteOffset, byteLength } as ViewExtent;
}

// The extent of a BufferSource: a view's (see viewByteExtent), or the whole
// of an ArrayBuffer or SharedArrayBuffer. Undefined for anything else.
function bufferSourceExtent(chunk: unknown): ViewExtent | undefined {
  if (isArrayBufferView(chunk)) return viewByteExtent(chunk);
  if (isArrayBuffer(chunk)) {
    return {
      __proto__: null,
      buffer: chunk,
      byteOffset: 0,
      byteLength: ArrayBufferPrototypeByteLengthGet(chunk),
    } as ViewExtent;
  }
  if (isSharedArrayBuffer(chunk)) {
    // ArrayBuffer.prototype.byteLength throws on a SharedArrayBuffer, so read
    // the length through a Uint8Array; shared memory cannot be detached.
    return {
      __proto__: null,
      buffer: chunk,
      byteOffset: 0,
      byteLength: TypedArrayPrototypeGetByteLength(new Uint8Array(chunk)),
    } as ViewExtent;
  }
  return undefined;
}

// Copies byteLength bytes of buffer, from byteOffset, into a new Uint8Array.
// An empty extent never touches buffer: a detached view's extent is empty,
// and constructing a view over a detached buffer throws.
function copyExtent(
  buffer: ArrayBufferLike,
  byteOffset: number,
  byteLength: number
): Uint8Array {
  const copy = new Uint8Array(byteLength);
  if (byteLength > 0) {
    TypedArrayPrototypeSet(
      copy,
      new Uint8Array(buffer, byteOffset, byteLength)
    );
  }
  return copy;
}

export type ViewExtentHelpers = {
  viewByteLength: typeof viewByteLength;
  viewByteExtent: typeof viewByteExtent;
  bufferSourceExtent: typeof bufferSourceExtent;
  copyExtent: typeof copyExtent;
};

module.exports = {
  viewByteLength,
  viewByteExtent,
  bufferSourceExtent,
  copyExtent,
};
