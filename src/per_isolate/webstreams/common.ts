'use strict';

// Helpers shared by the webstreams modules. Leaf module: requires nothing.

const { ObjectFreeze } = primordials;

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

export type CommonHelpers = {
  isActualObject: typeof isActualObject;
  kNullProtoObject: typeof kNullProtoObject;
  kEnumerable: typeof kEnumerable;
  toStringTagProperty: typeof toStringTagProperty;
};

module.exports = ObjectFreeze({
  isActualObject,
  kNullProtoObject,
  kEnumerable,
  toStringTagProperty,
});
