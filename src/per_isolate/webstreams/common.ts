'use strict';

// Helpers shared by the webstreams modules. Leaf module: requires nothing.

const { ObjectFreeze } = primordials;

function isActualObject(value: unknown): value is object {
  return value != null && typeof value === 'object';
}

export type CommonHelpers = {
  isActualObject: typeof isActualObject;
};

module.exports = ObjectFreeze({ isActualObject });
