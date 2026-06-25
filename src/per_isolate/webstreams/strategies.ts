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

class ByteLengthQueuingStrategy {
  #highWaterMark: number;

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
    if (!(#highWaterMark in this)) throw new TypeError('Illegal invocation');
    return this.#highWaterMark;
  }

  get size(): (chunk: ArrayBufferView) => number {
    if (!(#highWaterMark in this)) throw new TypeError('Illegal invocation');
    return byteLengthSize;
  }

  [SymbolToStringTag] = 'ByteLengthQueuingStrategy';
}

class CountQueuingStrategy {
  #highWaterMark: number;

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
    if (!(#highWaterMark in this)) throw new TypeError('Illegal invocation');
    return this.#highWaterMark;
  }

  get size(): () => number {
    if (!(#highWaterMark in this)) throw new TypeError('Illegal invocation');
    return countSize;
  }

  [SymbolToStringTag] = 'CountQueuingStrategy';
}

ObjectDefineProperties(ByteLengthQueuingStrategy.prototype, {
  highWaterMark: { enumerable: true },
  size: { enumerable: true },
});

ObjectDefineProperties(CountQueuingStrategy.prototype, {
  highWaterMark: { enumerable: true },
  size: { enumerable: true },
});

module.exports = {
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
};
