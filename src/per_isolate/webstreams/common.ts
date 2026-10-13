'use strict';

// Helpers shared by the webstreams modules. Leaf module: requires nothing.

import type { QueuingStrategy } from './types';

const {
  BigInt,
  NumberIsFinite,
  NumberIsNaN,
  ObjectFreeze,
  RangeError,
  TypeError,
  uncurryThis,
} = primordials;

function isActualObject(value: unknown): value is object {
  return value != null && typeof value === 'object';
}

// What an omitted (or null) dictionary argument stands for. Null-prototype:
// WebIDL reads nothing for an omitted dictionary, so neither may we.
const kNullProtoObject: object = ObjectFreeze({ __proto__: null });

const kEnumerable: PropertyDescriptor = ObjectFreeze({
  __proto__: kNullProtoObject,
  enumerable: true,
});

function toStringTagProperty(tag: string): PropertyDescriptor {
  return {
    __proto__: kNullProtoObject,
    value: tag,
    writable: false,
    enumerable: false,
    configurable: true,
  } as PropertyDescriptor;
}

const kMaxUint64 = 0xffff_ffff_ffff_ffffn;

// Converts the non-standard expectedLength extension: the total bytes a byte
// source or a transformer promises, or a FixedLengthStream's length. It
// converts as the C++ implementation converts a uint64_t, so the two accept
// the same values and agree on what they mean. undefined means no length. A
// bigint must fit in a uint64. Anything else goes through ToNumber (so '5' is
// 5, null is 0, and an object's valueOf runs), must be finite and
// non-negative, and is truncated (1.5 is 1). Every failure is a TypeError.
// One difference: C++'s range check lets the number 2 ** 64 through to a cast
// whose result is undefined; here it is a TypeError.
function normalizeExpectedLength(value: unknown): bigint | undefined {
  if (value === undefined) return undefined;
  if (typeof value === 'bigint') {
    if (value < 0n || value > kMaxUint64) {
      throw new TypeError('expectedLength must fit in a uint64');
    }
    return value;
  }
  // Unary plus is ToNumber. Number() would differ for an object whose
  // valueOf returns a bigint: it converts that, where ToNumber throws.
  const length = +(value as number);
  if (!NumberIsFinite(length) || length < 0 || length >= 2 ** 64) {
    throw new TypeError(
      'expectedLength must be a non-negative number that fits in a uint64'
    );
  }
  // Truncates, as length is finite and non-negative.
  return BigInt(length - (length % 1));
}

// WebIDL's check that a value is an object, a function included: null and
// every other primitive throw. Nothing is read. It converts an argument typed
// `object` (an underlying source or sink, a transformer), and a dictionary
// with a required member, which undefined and null cannot be.
function requireObject(value: unknown, message: string): void {
  if (!isActualObject(value) && typeof value !== 'function') {
    throw new TypeError(message);
  }
}

// WebIDL conversion of an optional dictionary argument (a strategy, an
// options bag), as in C++: undefined and null mean an empty dictionary, and
// anything else must pass requireObject.
function toDictionary(value: unknown, message: string): object {
  if (value === undefined || value === null) return kNullProtoObject;
  requireObject(value, message);
  return value as object;
}

function toStrategyDictionary(value: unknown): QueuingStrategy<unknown> {
  return toDictionary(
    value,
    'The queuing strategy must be an object.'
  ) as QueuingStrategy<unknown>;
}

// A strategy argument as WebIDL converts a QueuingStrategy dictionary: each
// member read once, in lexicographic order. highWaterMark has been through
// ToNumber; size is a function. An absent member is undefined.
interface QueuingStrategyDict {
  highWaterMark: number | undefined;
  size: ((chunk: unknown) => number) | undefined;
}

function convertQueuingStrategy(value: unknown): QueuingStrategyDict {
  const strategy = toStrategyDictionary(value);
  const rawHighWaterMark: unknown = strategy.highWaterMark;
  // Unary plus is ToNumber (see normalizeExpectedLength).
  const highWaterMark =
    rawHighWaterMark === undefined ? undefined : +(rawHighWaterMark as number);
  const size: unknown = strategy.size;
  if (size !== undefined && typeof size !== 'function') {
    throw new TypeError('strategy.size must be a function');
  }
  return {
    __proto__: null,
    highWaterMark,
    size,
  } as QueuingStrategyDict;
}

// ToNumber and the range check of the spec's ExtractHighWaterMark: NaN and
// negative numbers are a RangeError; Infinity and fractions are kept.
// Converting a number again has no side effects.
function convertHighWaterMark(value: unknown): number {
  const highWaterMark = +(value as number);
  if (NumberIsNaN(highWaterMark) || highWaterMark < 0) {
    throw new RangeError('Invalid highWaterMark');
  }
  return highWaterMark;
}

// The spec's ExtractHighWaterMark, on a converted strategy.
function extractHighWaterMark(
  strategy: QueuingStrategyDict,
  defaultHighWaterMark: number
): number {
  const { highWaterMark } = strategy;
  return highWaterMark === undefined
    ? defaultHighWaterMark
    : convertHighWaterMark(highWaterMark);
}

// The spec's ExtractSizeAlgorithm, on a converted strategy: size() runs with
// an undefined receiver and the chunk as its only argument. Without one,
// every chunk counts 1.
function extractSizeAlgorithm<T>(
  strategy: QueuingStrategyDict
): (chunk: T) => number {
  const { size } = strategy;
  if (size === undefined) return () => 1;
  const callSize = uncurryThis(size);
  return (chunk: T) => callSize(undefined, chunk);
}

export type CommonHelpers = {
  isActualObject: typeof isActualObject;
  kNullProtoObject: typeof kNullProtoObject;
  kEnumerable: typeof kEnumerable;
  toStringTagProperty: typeof toStringTagProperty;
  normalizeExpectedLength: typeof normalizeExpectedLength;
  requireObject: typeof requireObject;
  toDictionary: typeof toDictionary;
  toStrategyDictionary: typeof toStrategyDictionary;
  convertQueuingStrategy: typeof convertQueuingStrategy;
  convertHighWaterMark: typeof convertHighWaterMark;
  extractHighWaterMark: typeof extractHighWaterMark;
  extractSizeAlgorithm: typeof extractSizeAlgorithm;
};

module.exports = ObjectFreeze({
  isActualObject,
  kNullProtoObject,
  kEnumerable,
  toStringTagProperty,
  normalizeExpectedLength,
  requireObject,
  toDictionary,
  toStrategyDictionary,
  convertQueuingStrategy,
  convertHighWaterMark,
  extractHighWaterMark,
  extractSizeAlgorithm,
});
