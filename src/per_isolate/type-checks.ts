'use strict';

// Type checks shared by the bootstrap's modules. A leaf: it requires nothing.

// Whether `value` is an object in the language's sense (WebIDL's "is an
// Object"): anything but a primitive, so a function too.
function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

export type TypeCheckHelpers = {
  isObject: typeof isObject;
};

module.exports = {
  isObject,
};
