// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `Cloudflare.compatibilityFlags`: what it is, what it lists, and that
// nothing changes it.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { inspect } from 'node:util';
// The schema that defines the flags (io/compatibility-date.capnp), as text.
import schema from 'compatibility-date.capnp';
import { namedFlags } from 'which-impl';
import { attributesOf } from 'helpers';

const flags = Cloudflare.compatibilityFlags;

// Every enable-flag's name, in the order of the fields' numbers in the
// schema, which is not quite the order in which the file declares them.
const kEnableFlags = Array.from(
  schema.matchAll(/@(\d+)\s*:\s*Bool\b([^;]*);/g),
  (field) => [
    Number(field[1]),
    /\$compatEnableFlag\("([^"]+)"\)/.exec(field[2]),
  ]
)
  .filter(([, name]) => name !== null)
  .sort(([a], [b]) => a - b)
  .map(([, name]) => name[1]);
const kDisableFlags = Array.from(
  schema.matchAll(/\$compatDisableFlag\("([^"]+)"\)/g),
  (match) => match[1]
);

export const flagsShape = {
  test() {
    strictEqual(typeof flags, 'object');
    strictEqual(Object.getPrototypeOf(flags), null);
    strictEqual(Object.isExtensible(flags), false);
    strictEqual(Object.isSealed(flags), true);
    strictEqual(Object.isFrozen(flags), true);
    const keys = Reflect.ownKeys(flags);
    ok(keys.length > 100);
    for (const key of keys) {
      strictEqual(typeof key, 'string');
      // Read-only, enumerable, and not configurable.
      strictEqual(attributesOf(flags, key), 'datae', key);
      strictEqual(typeof flags[key], 'boolean', key);
    }
    deepStrictEqual(Object.keys(flags), keys);
  },
};

export const flagsListEveryEnableFlagInSchemaOrder = {
  test() {
    ok(kEnableFlags.length > 100);
    deepStrictEqual(Reflect.ownKeys(flags), kEnableFlags);
    const enumerated = [];
    for (const key in flags) enumerated.push(key);
    deepStrictEqual(enumerated, kEnableFlags);
    // A flag that is not set is listed as false; a disable-flag, like a name
    // that is no flag, is not listed.
    ok(Object.values(flags).includes(false));
    ok(kDisableFlags.length > 0);
    for (const name of kDisableFlags) {
      strictEqual(name in flags, false, name);
      strictEqual(flags[name], undefined, name);
    }
    strictEqual('not_a_real_compat_flag' in flags, false);
    strictEqual(flags['not_a_real_compat_flag'], undefined);
    // Nor anything Object.prototype has.
    strictEqual(flags.toString, undefined);
    strictEqual('constructor' in flags, false);
  },
};

export const flagsAreTheSameObject = {
  test() {
    strictEqual(Cloudflare.compatibilityFlags, flags);
    strictEqual(
      Object.getOwnPropertyDescriptor(Cloudflare, 'compatibilityFlags').value,
      flags
    );
    const { compatibilityFlags } = globalThis.Cloudflare;
    strictEqual(compatibilityFlags, flags);
  },
};

export const flagsCannotChange = {
  test() {
    const before = JSON.stringify(flags);
    const name = 'formdata_parser_supports_files';
    strictEqual(name in flags, true);
    const strict = [
      () => {
        'use strict';
        flags[name] = !flags[name];
      },
      () => {
        'use strict';
        flags.not_a_real_compat_flag = true;
      },
      () => {
        'use strict';
        delete flags.formdata_parser_supports_files;
      },
      () => Object.defineProperty(flags, name, { value: !flags[name] }),
      () => Object.defineProperty(flags, 'extra', { value: 1 }),
      () => Object.setPrototypeOf(flags, Object.prototype),
    ];
    for (const change of strict) throws(change, TypeError);
    // Reflect reports the failure instead.
    strictEqual(Reflect.set(flags, name, !flags[name]), false);
    strictEqual(Reflect.set(flags, 'extra', 1), false);
    strictEqual(Reflect.deleteProperty(flags, name), false);
    strictEqual(Reflect.defineProperty(flags, 'extra', { value: 1 }), false);
    strictEqual(Reflect.setPrototypeOf(flags, Object.prototype), false);
    strictEqual(JSON.stringify(flags), before);
    deepStrictEqual(Reflect.ownKeys(flags), kEnableFlags);
    strictEqual(Object.getPrototypeOf(flags), null);
  },
};

export const flagValues = {
  test() {
    // The flags the cell's config names are set, whatever the date.
    ok(namedFlags.length > 0);
    for (const name of namedFlags) strictEqual(flags[name], true, name);
    // A flag says what the runtime does. With global_navigator there is a
    // `navigator`; with set_tostring_tag an API object has a tag.
    strictEqual(flags['global_navigator'], typeof navigator !== 'undefined');
    strictEqual(
      flags['set_tostring_tag'],
      Object.prototype.toString.call(new Headers()) === '[object Headers]'
    );
  },
};

export const flagsReadAsAnyObject = {
  test() {
    const copy = { ...flags };
    deepStrictEqual(Object.keys(copy), kEnableFlags);
    strictEqual(Object.getPrototypeOf(copy), Object.prototype);
    deepStrictEqual(
      Object.entries(flags).map(([name]) => name),
      kEnableFlags
    );
    deepStrictEqual(Object.keys(JSON.parse(JSON.stringify(flags))), [
      ...kEnableFlags,
    ]);
    ok(inspect(flags).startsWith('[Object: null prototype] {'));
  },
};

export const flagsClone = {
  test() {
    // A plain object to the serializer, which gives the copy a prototype and
    // leaves it unfrozen.
    const clone = structuredClone(flags);
    ok(clone !== flags);
    strictEqual(Object.getPrototypeOf(clone), Object.prototype);
    strictEqual(Object.isFrozen(clone), false);
    deepStrictEqual(Object.keys(clone), kEnableFlags);
    for (const name of kEnableFlags) strictEqual(clone[name], flags[name]);
  },
};
