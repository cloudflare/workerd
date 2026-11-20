// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The attributes: their values, and how they behave as the instance's own
// read-only properties.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { hasLanguage, hasStorage } from 'which-impl';
import { attributesOf, throwsTypeError } from 'helpers';

const kNames = [
  ...(hasStorage ? ['storage'] : []),
  ...(hasLanguage ? ['languages', 'language'] : []),
  'platform',
  'hardwareConcurrency',
  'userAgent',
];

export const values = {
  test() {
    strictEqual(navigator.userAgent, 'Cloudflare-Workers');
    strictEqual(navigator.hardwareConcurrency, 1);
    strictEqual(navigator.platform, '');
    if (hasLanguage) {
      strictEqual(navigator.language, 'en');
      deepStrictEqual(navigator.languages, ['en']);
    } else {
      strictEqual(navigator.language, undefined);
      strictEqual(navigator.languages, undefined);
      strictEqual('language' in navigator, false);
      strictEqual('languages' in navigator, false);
    }
    if (hasStorage) {
      strictEqual(typeof navigator.storage, 'object');
    } else {
      strictEqual(navigator.storage, undefined);
      strictEqual('storage' in navigator, false);
    }
  },
};

export const readOnly = {
  test() {
    for (const name of kNames) {
      const before = Object.getOwnPropertyDescriptor(navigator, name).value;
      throwsTypeError(() => {
        'use strict';
        navigator[name] = 'changed';
      }, `Cannot assign to read only property '${name}' of object '#<Navigator>'`);
      strictEqual(Reflect.set(navigator, name, 'changed'), false);
      if (name !== 'languages') strictEqual(navigator[name], before);
      strictEqual(attributesOf(navigator, name), 'dataec');
    }
    deepStrictEqual(Reflect.ownKeys(navigator), kNames);
  },
};

export const languagesIsANewArrayEachTime = {
  test() {
    if (!hasLanguage) return;
    const first = navigator.languages;
    const second = navigator.languages;
    ok(first !== second);
    ok(Array.isArray(first));
    strictEqual(Object.getPrototypeOf(first), Array.prototype);
    strictEqual(Object.isFrozen(first), false);
    deepStrictEqual(Reflect.ownKeys(first), ['0', 'length']);
    // So changing one changes no other.
    first.push('fr');
    first[0] = 'de';
    deepStrictEqual(first, ['de', 'fr']);
    deepStrictEqual(second, ['en']);
    deepStrictEqual(navigator.languages, ['en']);
    // The descriptor's value is one more new array.
    const descriptor = Object.getOwnPropertyDescriptor(navigator, 'languages');
    deepStrictEqual(descriptor.value, ['en']);
    ok(descriptor.value !== navigator.languages);
    // As is what an object inheriting from the navigator reads.
    const derived = Object.create(navigator);
    deepStrictEqual(derived.languages, ['en']);
    ok(derived.languages !== derived.languages);
  },
};

export const storageIsTheSameObject = {
  test() {
    if (!hasStorage) return;
    const storage = navigator.storage;
    strictEqual(navigator.storage, storage);
    strictEqual(storage instanceof StorageManager, true);
    strictEqual(Object.getPrototypeOf(storage), StorageManager.prototype);
    strictEqual(
      Object.getOwnPropertyDescriptor(navigator, 'storage').value,
      storage
    );
    strictEqual(Object.create(navigator).storage, storage);
    strictEqual(typeof storage.getDirectory, 'function');
  },
};

export const storageWorks = {
  async test() {
    if (!hasStorage) return;
    const root = await navigator.storage.getDirectory();
    strictEqual(root instanceof FileSystemDirectoryHandle, true);
  },
};

export const inherited = {
  test() {
    // The attributes are data properties, so an object that inherits from
    // the navigator reads them.
    const derived = Object.create(navigator);
    deepStrictEqual(Reflect.ownKeys(derived), []);
    for (const name of kNames) {
      if (name === 'languages') continue;
      strictEqual(derived[name], navigator[name], name);
    }
    // And, as for any inherited read-only property, cannot assign them.
    throws(() => {
      'use strict';
      derived.userAgent = 'changed';
    }, TypeError);
    deepStrictEqual(Reflect.ownKeys(derived), []);
  },
};

export const json = {
  test() {
    const parsed = JSON.parse(JSON.stringify(navigator));
    strictEqual(parsed.userAgent, 'Cloudflare-Workers');
    strictEqual(parsed.hardwareConcurrency, 1);
    strictEqual(parsed.platform, '');
    strictEqual(parsed.language, hasLanguage ? 'en' : undefined);
    if (hasStorage) deepStrictEqual(parsed.storage, {});
    strictEqual('sendBeacon' in parsed, false);
  },
};

export const notSerializable = {
  test() {
    throws(
      () => structuredClone(navigator),
      (error) => {
        ok(error instanceof DOMException);
        strictEqual(error.name, 'DataCloneError');
        strictEqual(
          error.message,
          'Could not serialize object of type "Navigator". This type does ' +
            'not support serialization.'
        );
        return true;
      }
    );
    // An object that inherits from it is an ordinary object.
    const clone = structuredClone(Object.create(navigator));
    deepStrictEqual(Reflect.ownKeys(clone), []);
    strictEqual(Object.getPrototypeOf(clone), Object.prototype);
  },
};
