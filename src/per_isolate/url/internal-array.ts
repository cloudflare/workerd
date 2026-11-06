'use strict';

// A leaf. User code can define index setters on Array.prototype, and
// Array.prototype.push(), like any write past an array's end, runs an
// inherited setter rather than defining the element. So the URL modules
// build their lists in arrays with a null prototype, and hand user code an
// ordinary copy: Array.prototype.slice() defines its result's elements.

const { ArrayPrototypeSlice, ObjectSetPrototypeOf } = primordials;

function internalArray<T>(): T[] {
  return ObjectSetPrototypeOf([], null) as T[];
}

// An ordinary array of `list`'s elements.
function toUserArray<T>(list: readonly T[]): T[] {
  return ArrayPrototypeSlice(list) as T[];
}

export type InternalArrayHelpers = {
  internalArray: typeof internalArray;
  toUserArray: typeof toUserArray;
};

module.exports = {
  internalArray,
  toUserArray,
};
