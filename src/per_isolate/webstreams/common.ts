'use strict';

// Helpers shared by the webstreams modules. Leaf module: requires nothing.

const { BigInt, NumberIsFinite, ObjectFreeze, TypeError } = primordials;

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

export type CommonHelpers = {
  isActualObject: typeof isActualObject;
  kNullProtoObject: typeof kNullProtoObject;
  kEnumerable: typeof kEnumerable;
  toStringTagProperty: typeof toStringTagProperty;
  normalizeExpectedLength: typeof normalizeExpectedLength;
};

module.exports = ObjectFreeze({
  isActualObject,
  kNullProtoObject,
  kEnumerable,
  toStringTagProperty,
  normalizeExpectedLength,
});
