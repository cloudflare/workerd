// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Event and CustomEvent under a polluted environment. WebIDL reads a user's
// dictionary with [[Get]], so inherited members count, but an omitted
// dictionary reads nothing; and construction never goes through patchable
// built-ins.

import { notStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Defines the entries on Object.prototype; returns a function removing them.
// The descriptors are null-prototype: once `get` is polluted, an ordinary
// one would inherit it.
function pollute(entries) {
  for (const key of Object.keys(entries)) {
    Object.defineProperty(Object.prototype, key, {
      __proto__: null,
      value: entries[key],
      configurable: true,
      writable: true,
    });
  }
  return () => {
    for (const key of Object.keys(entries)) {
      Reflect.deleteProperty(Object.prototype, key);
    }
  };
}

function withPollutedInitMembers(fn) {
  const restore = pollute({
    bubbles: true,
    cancelable: true,
    composed: true,
    detail: 'polluted',
  });
  try {
    fn();
  } finally {
    restore();
  }
}

export const omittedInitReadsNothing = {
  test() {
    withPollutedInitMembers(() => {
      for (const event of [new Event('x'), new CustomEvent('x')]) {
        strictEqual(event.bubbles, false);
        strictEqual(event.cancelable, false);
        strictEqual(event.composed, false);
      }
      // The default diverges as ever (#10).
      strictEqual(new CustomEvent('x').detail, usingTsImpl ? null : undefined);
    });
  },
};

export const inheritedInitMembersAreRead = {
  test() {
    withPollutedInitMembers(() => {
      for (const event of [new Event('x', {}), new CustomEvent('x', {})]) {
        strictEqual(event.bubbles, true);
        strictEqual(event.cancelable, true);
        strictEqual(event.composed, true);
      }
      strictEqual(new CustomEvent('x', {}).detail, 'polluted');
    });
  },
};

// A descriptor that inherited these would mix accessor and data fields.
export const descriptorPollutionDoesNotReachIsTrusted = {
  test() {
    const restore = pollute({
      get: () => true,
      set: () => {},
      value: true,
      writable: true,
      configurable: true,
    });
    let event;
    try {
      event = new Event('x');
    } finally {
      restore();
    }
    strictEqual(event.isTrusted, false);
    if (usingTsImpl) {
      const { get, configurable } = Object.getOwnPropertyDescriptor(
        event,
        'isTrusted'
      );
      strictEqual(get.name, 'get isTrusted');
      strictEqual(configurable, false);
    }
  },
};

export const patchedBuiltinsAreNotUsed = {
  test() {
    const { now } = Date;
    const { defineProperty } = Object;
    Date.now = () => 42;
    Object.defineProperty = () => {
      throw new Error('patched Object.defineProperty');
    };
    let event;
    let custom;
    try {
      event = new Event('x', { cancelable: true });
      custom = new CustomEvent('y', { detail: 1 });
    } finally {
      Date.now = now;
      Object.defineProperty = defineProperty;
    }
    notStrictEqual(event.timeStamp, 42);
    strictEqual(event.isTrusted, false);
    strictEqual(event.cancelable, true);
    strictEqual(custom.detail, 1);
  },
};
