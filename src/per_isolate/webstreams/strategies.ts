'use strict';

// ByteLengthQueuingStrategy and CountQueuingStrategy (WHATWG Streams §7).

const { ObjectDefineProperties, SymbolToStringTag, TypeError } = primordials;

function isActualObject(value: unknown): boolean {
  return value != null && typeof value === 'object';
}

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

  // The init type is optional-shaped because user input is arbitrary; the
  // required-member check is the explicit TypeError below.
  constructor(init: { highWaterMark?: number }) {
    if (!isActualObject(init)) {
      throw new TypeError('init must be an object');
    }
    if (init.highWaterMark === undefined) {
      throw new TypeError('init.highWaterMark is required');
    }
    // Stored as-converted WITHOUT range validation: per spec (and WPT),
    // bogus values like NaN or -1 are accepted here and only rejected by
    // ExtractHighWaterMark at stream construction time.
    this.#highWaterMark = +init.highWaterMark;
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
    if (!isActualObject(init)) {
      throw new TypeError('init must be an object');
    }
    if (init.highWaterMark === undefined) {
      throw new TypeError('init.highWaterMark is required');
    }
    this.#highWaterMark = +init.highWaterMark;
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

const kEnumerable = { __proto__: null, enumerable: true };

ObjectDefineProperties(ByteLengthQueuingStrategy.prototype, {
  __proto__: null,
  highWaterMark: kEnumerable,
  size: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'ByteLengthQueuingStrategy',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

ObjectDefineProperties(CountQueuingStrategy.prototype, {
  __proto__: null,
  highWaterMark: kEnumerable,
  size: kEnumerable,
  [SymbolToStringTag]: {
    __proto__: null,
    value: 'CountQueuingStrategy',
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

module.exports = {
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
};
