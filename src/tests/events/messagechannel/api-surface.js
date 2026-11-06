// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object shape of MessageChannel and MessagePort: members, attributes,
// prototype chains and the marks JSG puts on every runtime class.
// Divergences (see the ledger in AGENTS.md):
// - #1: only the TypeScript MessagePort constructor inherits from
//   EventTarget's, as WebIDL requires.
// - #2: the TypeScript constructors' `prototype` is non-writable even
//   without immutable_api_prototypes.
// - #5: `constructor` comes first among MessagePort.prototype's own keys in
//   the TypeScript class, last among the string keys in C++.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import * as workerThreads from 'node:worker_threads';
import { attributes } from 'helpers';
import {
  immutableApiPrototypes,
  setToStringTag,
  specCompliantPropertyAttributes,
  usingTsImpl,
} from 'which-impl';

export const prototypeMethods = {
  test() {
    for (const name of ['postMessage', 'close', 'start']) {
      const descriptor = Object.getOwnPropertyDescriptor(
        MessagePort.prototype,
        name
      );
      strictEqual(attributes(descriptor), 'datawec');
      strictEqual(descriptor.value.name, name);
      // No argument is required: JSG counts none of the optional ones.
      strictEqual(descriptor.value.length, 0);
      throws(() => new descriptor.value(), TypeError);
    }
  },
};

export const onmessageAccessor = {
  test() {
    const { get, set, ...rest } = Object.getOwnPropertyDescriptor(
      MessagePort.prototype,
      'onmessage'
    );
    strictEqual(attributes({ get, set, ...rest }), 'accessorec');
    strictEqual(
      get.name,
      specCompliantPropertyAttributes ? 'get onmessage' : ''
    );
    strictEqual(
      set.name,
      specCompliantPropertyAttributes ? 'set onmessage' : ''
    );
    strictEqual(get.length, 0);
    strictEqual(set.length, specCompliantPropertyAttributes ? 1 : 0);
  },
};

export const prototypeKeys = {
  test() {
    const keys = Reflect.ownKeys(MessagePort.prototype).filter(
      (key) => typeof key === 'string'
    );
    deepStrictEqual(
      keys,
      usingTsImpl
        ? ['constructor', 'postMessage', 'close', 'start', 'onmessage']
        : ['postMessage', 'close', 'start', 'onmessage', 'constructor']
    );
    deepStrictEqual(
      Reflect.ownKeys(MessageChannel.prototype).filter(
        (key) => typeof key === 'string'
      ),
      ['constructor']
    );
  },
};

export const prototypeChains = {
  test() {
    strictEqual(
      Object.getPrototypeOf(MessagePort.prototype),
      EventTarget.prototype
    );
    strictEqual(
      Object.getPrototypeOf(MessageChannel.prototype),
      Object.prototype
    );
    strictEqual(
      Object.getPrototypeOf(MessagePort),
      usingTsImpl ? EventTarget : Function.prototype
    );
    strictEqual(Object.getPrototypeOf(MessageChannel), Function.prototype);
  },
};

export const constructorAttributes = {
  test() {
    for (const ctor of [MessagePort, MessageChannel]) {
      strictEqual(ctor.length, 0);
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(ctor, 'length')),
        'datac'
      );
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(ctor, 'name')),
        'datac'
      );
      strictEqual(
        attributes(
          Object.getOwnPropertyDescriptor(ctor.prototype, 'constructor')
        ),
        'datawc'
      );
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(ctor, 'prototype')),
        usingTsImpl || immutableApiPrototypes ? 'data' : 'dataw'
      );
    }
  },
};

export const runtimeClassMarks = {
  test() {
    for (const proto of [MessagePort.prototype, MessageChannel.prototype]) {
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
    }
  },
};

export const toStringTagBranding = {
  test() {
    const channel = new MessageChannel();
    for (const [object, name] of [
      [channel.port1, 'MessagePort'],
      [channel, 'MessageChannel'],
    ]) {
      const proto = Object.getPrototypeOf(object);
      const tag = Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag);
      if (setToStringTag) {
        strictEqual(attributes(tag), 'datawc');
        strictEqual(tag.value, name);
        strictEqual(Object.prototype.toString.call(object), `[object ${name}]`);
      } else {
        strictEqual(tag, undefined);
        strictEqual(Object.prototype.toString.call(object), '[object Object]');
      }
    }
  },
};

export const channelOwnProperties = {
  test() {
    const channel = new MessageChannel();
    // In the order JSG's lazy instance properties report them.
    deepStrictEqual(Reflect.ownKeys(channel), ['port2', 'port1']);
    for (const name of ['port1', 'port2']) {
      const descriptor = Object.getOwnPropertyDescriptor(channel, name);
      strictEqual(attributes(descriptor), 'dataec');
      ok(descriptor.value instanceof MessagePort);
    }
    ok(channel.port1 !== channel.port2);
    // A port has no own properties, its brand included.
    deepStrictEqual(Reflect.ownKeys(channel.port1), []);
    channel.port1.close();
  },
};

export const globalsAndWorkerThreads = {
  test() {
    for (const name of ['MessageChannel', 'MessagePort']) {
      ok(name in globalThis);
      // Always on the global scope's prototype, and also own properties of
      // the global under spec_compliant_property_attributes.
      const inherited = Object.getOwnPropertyDescriptor(
        Object.getPrototypeOf(globalThis),
        name
      );
      strictEqual(attributes(inherited), 'datawec');
      const own = Object.getOwnPropertyDescriptor(globalThis, name);
      if (specCompliantPropertyAttributes) {
        strictEqual(attributes(own), 'datawc');
      } else {
        strictEqual(own, undefined);
      }
    }
    // node:worker_threads hands out the same classes.
    strictEqual(workerThreads.MessageChannel, MessageChannel);
    strictEqual(workerThreads.MessagePort, MessagePort);
  },
};
