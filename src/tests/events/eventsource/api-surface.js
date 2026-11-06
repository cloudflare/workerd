// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object shape of EventSource: members, attributes, prototype chains and
// the marks JSG puts on every runtime class. Divergences (see the ledger in
// AGENTS.md):
// - #1: only the TypeScript constructor inherits from EventTarget's, as
//   WebIDL requires.
// - #2: the TypeScript constructor's `prototype` is non-writable even
//   without immutable_api_prototypes.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { attributes, streamOf } from 'helpers';
import {
  immutableApiPrototypes,
  setToStringTag,
  specCompliantPropertyAttributes,
  usingTsImpl,
} from 'which-impl';

// WebIDL constants are non-configurable; JSG's are configurable unless
// spec_compliant_property_attributes.
const kConstantAttributes = specCompliantPropertyAttributes
  ? 'datae'
  : 'dataec';
// Lengths count required arguments under spec_compliant_property_attributes.
const kRequiredLength = specCompliantPropertyAttributes ? 1 : 0;

const kConstants = [
  ['CONNECTING', 0],
  ['OPEN', 1],
  ['CLOSED', 2],
];

export const staticMembers = {
  test() {
    deepStrictEqual(Reflect.ownKeys(EventSource), [
      'length',
      'name',
      'prototype',
      'CONNECTING',
      'OPEN',
      'CLOSED',
      'from',
    ]);
    for (const [name, value] of kConstants) {
      const descriptor = Object.getOwnPropertyDescriptor(EventSource, name);
      strictEqual(attributes(descriptor), kConstantAttributes);
      strictEqual(descriptor.value, value);
    }
    const from = Object.getOwnPropertyDescriptor(EventSource, 'from');
    strictEqual(attributes(from), 'datawec');
    strictEqual(from.value.name, 'from');
    strictEqual(from.value.length, kRequiredLength);
  },
};

export const prototypeMembers = {
  test() {
    const proto = EventSource.prototype;
    const close = Object.getOwnPropertyDescriptor(proto, 'close');
    strictEqual(attributes(close), 'datawec');
    strictEqual(close.value.name, 'close');
    strictEqual(close.value.length, 0);
    throws(() => new close.value(), TypeError);

    for (const name of ['url', 'withCredentials', 'readyState']) {
      const descriptor = Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(attributes(descriptor), 'accessorec');
      strictEqual(descriptor.set, undefined);
      strictEqual(
        descriptor.get.name,
        specCompliantPropertyAttributes ? `get ${name}` : ''
      );
      strictEqual(descriptor.get.length, 0);
    }
    for (const name of ['onopen', 'onmessage', 'onerror']) {
      const { get, set, ...rest } = Object.getOwnPropertyDescriptor(
        proto,
        name
      );
      strictEqual(attributes({ get, set, ...rest }), 'accessorec');
      strictEqual(
        get.name,
        specCompliantPropertyAttributes ? `get ${name}` : ''
      );
      strictEqual(
        set.name,
        specCompliantPropertyAttributes ? `set ${name}` : ''
      );
      strictEqual(get.length, 0);
      strictEqual(set.length, specCompliantPropertyAttributes ? 1 : 0);
    }
    for (const [name, value] of kConstants) {
      const descriptor = Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(attributes(descriptor), kConstantAttributes);
      strictEqual(descriptor.value, value);
    }
  },
};

export const prototypeKeys = {
  test() {
    deepStrictEqual(
      Reflect.ownKeys(EventSource.prototype).filter(
        (key) => typeof key === 'string'
      ),
      [
        'close',
        'url',
        'withCredentials',
        'readyState',
        'onopen',
        'onmessage',
        'onerror',
        'CONNECTING',
        'OPEN',
        'CLOSED',
        'constructor',
      ]
    );
  },
};

export const prototypeChains = {
  test() {
    strictEqual(
      Object.getPrototypeOf(EventSource.prototype),
      EventTarget.prototype
    );
    strictEqual(
      Object.getPrototypeOf(EventSource),
      usingTsImpl ? EventTarget : Function.prototype
    );
  },
};

export const constructorAttributes = {
  test() {
    strictEqual(EventSource.length, kRequiredLength);
    strictEqual(EventSource.name, 'EventSource');
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(EventSource, 'length')),
      'datac'
    );
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(EventSource, 'name')),
      'datac'
    );
    const constructor = Object.getOwnPropertyDescriptor(
      EventSource.prototype,
      'constructor'
    );
    strictEqual(attributes(constructor), 'datawc');
    strictEqual(constructor.value, EventSource);
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(EventSource, 'prototype')),
      usingTsImpl || immutableApiPrototypes ? 'data' : 'dataw'
    );
  },
};

export const runtimeClassMarks = {
  test() {
    const proto = EventSource.prototype;
    // The same marks, with the same attributes, as EventTarget's.
    for (const description of [
      'cloudflare:internal-class',
      'kResourceTypeInspect',
    ]) {
      const find = (obj) =>
        Object.getOwnPropertySymbols(obj).find(
          (symbol) => symbol.description === description
        );
      const symbol = find(proto);
      ok(symbol, description);
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(proto, symbol)),
        attributes(
          Object.getOwnPropertyDescriptor(
            EventTarget.prototype,
            find(EventTarget.prototype)
          )
        )
      );
    }
    strictEqual(
      proto[Symbol.for('cloudflare:internal-class')],
      Symbol.for('cloudflare:internal-class')
    );
  },
};

export const toStringTagBranding = {
  test() {
    const source = EventSource.from(streamOf([], { close: false }));
    const tag = Object.getOwnPropertyDescriptor(
      EventSource.prototype,
      Symbol.toStringTag
    );
    if (setToStringTag) {
      strictEqual(attributes(tag), 'datawc');
      strictEqual(tag.value, 'EventSource');
      strictEqual(
        Object.prototype.toString.call(source),
        '[object EventSource]'
      );
    } else {
      strictEqual(tag, undefined);
      strictEqual(Object.prototype.toString.call(source), '[object Object]');
    }
    source.close();
  },
};

// Disposal defers to close() through a function of its own.
export const disposeMember = {
  test() {
    const dispose = Object.getOwnPropertyDescriptor(
      EventSource.prototype,
      Symbol.dispose
    );
    strictEqual(attributes(dispose), 'datawc');
    strictEqual(dispose.value.name, '');
    strictEqual(dispose.value.length, 0);
    ok(dispose.value !== EventSource.prototype.close);
  },
};

export const instancesHaveNoOwnProperties = {
  test() {
    const source = EventSource.from(streamOf([], { close: false }));
    deepStrictEqual(Reflect.ownKeys(source), []);
    source.close();
  },
};

export const globalProperty = {
  test() {
    ok('EventSource' in globalThis);
    // Always on the global scope's prototype, and also an own property of the
    // global under spec_compliant_property_attributes.
    const inherited = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(globalThis),
      'EventSource'
    );
    strictEqual(attributes(inherited), 'datawec');
    const own = Object.getOwnPropertyDescriptor(globalThis, 'EventSource');
    if (specCompliantPropertyAttributes) {
      strictEqual(attributes(own), 'datawc');
    } else {
      strictEqual(own, undefined);
    }
  },
};
