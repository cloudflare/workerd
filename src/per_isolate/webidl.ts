'use strict';

// WebIDL helpers shared by the bootstrap's classes.

// Whether `value` is an Object in WebIDL's sense: an object or a function.
function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

export type WebIdlHelpers = {
  isObject: typeof isObject;
};

module.exports = {
  isObject,
};
