'use strict';

// ByteLengthQueuingStrategy and CountQueuingStrategy (WHATWG Streams §7).

import type { CommonHelpers } from './common';

const { ObjectDefineProperties, SymbolToStringTag, TypeError } = primordials;

const { isActualObject, kEnumerable, requireObject, toStringTagProperty } =
  require('webstreams/common') as CommonHelpers;

// Per spec, the size function is the SAME function object for every
// strategy instance in the realm, and its name is 'size' (method shorthand
// gives the right .name and .length). The byteLength read is a deliberate,
// user-observable property access — that IS the specified behavior (the
// chunk is arbitrary user data, not a trusted view).
const byteLengthSize = {
  size(chunk: ArrayBufferView): number {
    return chunk.byteLength;
  },
}.size;

const countSize = {
  size(): number {
    return 1;
  },
}.size;

// Converts init as WebIDL converts a QueuingStrategyInit dictionary, to its
// one member. The type is optional-shaped because user input is arbitrary;
// the required-member check is the explicit TypeError below. A dictionary
// with a required member cannot be converted from undefined or null.
function convertQueuingStrategyInit(init: { highWaterMark?: number }): number {
  requireObject(init, 'init must be an object');
  // Read once, as WebIDL dictionary conversion reads each member once.
  const highWaterMark = init.highWaterMark;
  if (highWaterMark === undefined) {
    throw new TypeError('init.highWaterMark is required');
  }
  // No range validation: per spec (and WPT), bogus values like NaN or -1
  // are accepted here and only rejected by ExtractHighWaterMark at stream
  // construction time.
  return +highWaterMark;
}

let assertIsByteLengthQueuingStrategy: (
  value: ByteLengthQueuingStrategy
) => void;
let assertIsCountQueuingStrategy: (value: CountQueuingStrategy) => void;

class ByteLengthQueuingStrategy {
  #highWaterMark: number;

  static {
    assertIsByteLengthQueuingStrategy = function (
      self: ByteLengthQueuingStrategy
    ) {
      if (!isActualObject(self) || !(#highWaterMark in self))
        throw new TypeError('Illegal invocation');
    };
  }

  constructor(init: { highWaterMark?: number }) {
    this.#highWaterMark = convertQueuingStrategyInit(init);
  }

  get highWaterMark(): number {
    assertIsByteLengthQueuingStrategy(this);
    return this.#highWaterMark;
  }

  get size(): (chunk: ArrayBufferView) => number {
    assertIsByteLengthQueuingStrategy(this);
    return byteLengthSize;
  }
}

class CountQueuingStrategy {
  #highWaterMark: number;

  static {
    assertIsCountQueuingStrategy = function (self: CountQueuingStrategy) {
      if (!isActualObject(self) || !(#highWaterMark in self))
        throw new TypeError('Illegal invocation');
    };
  }

  constructor(init: { highWaterMark?: number }) {
    this.#highWaterMark = convertQueuingStrategyInit(init);
  }

  get highWaterMark(): number {
    assertIsCountQueuingStrategy(this);
    return this.#highWaterMark;
  }

  get size(): () => number {
    assertIsCountQueuingStrategy(this);
    return countSize;
  }
}

ObjectDefineProperties(ByteLengthQueuingStrategy.prototype, {
  __proto__: null,
  highWaterMark: kEnumerable,
  size: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('ByteLengthQueuingStrategy'),
});

ObjectDefineProperties(CountQueuingStrategy.prototype, {
  __proto__: null,
  highWaterMark: kEnumerable,
  size: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('CountQueuingStrategy'),
});

module.exports = {
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
};
