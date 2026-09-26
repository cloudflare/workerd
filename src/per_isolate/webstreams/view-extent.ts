'use strict';

// The byte extent of an ArrayBufferView, read through captured getters, for
// code that consumes caller-supplied views (the internal transform pairs'
// write paths, the digest stream, byte-counting strategies, body
// collection).
//
// A view whose buffer has been detached, or that a resizable buffer has
// shrunk out from under, has no bytes: the typed-array getters report
// byteOffset and byteLength 0 for it, but the DataView getters throw a
// TypeError. These helpers give a DataView the typed arrays' answer, so
// such a view reads as empty whichever kind it is — as it does in the C++
// implementation, which treats it as zero-length. Leaf module: requires
// nothing.

const {
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  TypedArrayPrototypeGetSymbolToStringTag,
} = primordials;

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

export type ViewExtentHelpers = {
  viewByteLength: typeof viewByteLength;
  viewByteExtent: typeof viewByteExtent;
};

module.exports = { viewByteLength, viewByteExtent };
