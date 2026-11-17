'use strict';

// Internal testing only: cloneable classes (see serialization.ts), which
// main.ts exposes under the expose_serialization_test_class flag, so that
// tests can send them through structuredClone() and RPC: CloneableTestBox,
// holding a label and any serializable value, and CloneableTestError, an
// error whose stack the serializer handles. As for any class that only some
// flags expose, the module is loaded only under the flag, so a reader
// without the flag can't read one.

import type { SerializationHelpers } from '../serialization';

const { ObjectDefineProperty, TypeError } = primordials;
const { getPrivateSymbol } = utils;
const { getOwnDataProperty, makeCloneable, markCloneable } =
  require('serialization') as SerializationHelpers;

const kLabel = getPrivateSymbol('testing:CloneableTestBox:label');
const kValue = getPrivateSymbol('testing:CloneableTestBox:value');

type Slots = Record<symbol, unknown>;

function initBox(box: CloneableTestBox, label: unknown, value: unknown): void {
  if (typeof label !== 'string') {
    throw new TypeError('CloneableTestBox label must be a string');
  }
  (box as unknown as Slots)[kLabel] = label;
  (box as unknown as Slots)[kValue] = value;
}

function slotsOf(value: unknown): Slots {
  if (
    typeof value !== 'object' ||
    value === null ||
    (value as Slots)[kLabel] === undefined
  ) {
    throw new TypeError('Illegal invocation');
  }
  return value as Slots;
}

class CloneableTestBox {
  constructor(label: string, value?: unknown) {
    initBox(this, label, value);
    markCloneable(this, boxCloneable);
  }

  get label(): string {
    return slotsOf(this)[kLabel] as string;
  }

  get value(): unknown {
    return slotsOf(this)[kValue];
  }
}

const boxCloneable = makeCloneable(
  CloneableTestBox,
  'testing/cloneable-test-box:CloneableTestBox',
  (box) => {
    const slots = slotsOf(box);
    return { label: slots[kLabel], value: slots[kValue] };
  },
  (box, data) => {
    if (typeof data !== 'object' || data === null) {
      throw new TypeError('Invalid CloneableTestBox data');
    }
    initBox(
      box,
      getOwnDataProperty(data, 'label'),
      getOwnDataProperty(data, 'value')
    );
  }
);

class CloneableTestError extends Error {
  constructor(message: string) {
    super(message);
    markCloneable(this, errorCloneable);
  }
}

// As Error.prototype.name is defined, so that a stack's first line names the
// class.
ObjectDefineProperty(CloneableTestError.prototype, 'name', {
  __proto__: null,
  value: 'CloneableTestError',
  writable: true,
  configurable: true,
});

// The data is the message. The serializer writes the stack.
const errorCloneable = makeCloneable(
  CloneableTestError,
  'testing/cloneable-test-box:CloneableTestError',
  (error) => getOwnDataProperty(error, 'message'),
  (error, data) => {
    if (typeof data !== 'string') {
      throw new TypeError('Invalid CloneableTestError data');
    }
    ObjectDefineProperty(error, 'message', {
      __proto__: null,
      value: data,
      writable: true,
      configurable: true,
    });
  }
);

module.exports = { CloneableTestBox, CloneableTestError };
