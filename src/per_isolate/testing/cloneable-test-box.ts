'use strict';

// Internal testing only: a cloneable class (see serialization.ts) holding a
// label and any serializable value, which main.ts exposes as
// CloneableTestBox under the expose_serialization_test_class flag, so that
// tests can send one through structuredClone() and RPC. As for any class
// that only some flags expose, it is loaded only under the flag, so a reader
// without the flag can't read one.

import type { SerializationHelpers } from '../serialization';

const { TypeError } = primordials;
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

module.exports = { CloneableTestBox };
