'use strict';

// The `Cloudflare` global, under the TS_CLOUDFLARE_GLOBAL autogate. It
// replaces the C++ Cloudflare class in api/global-scope.h, with the shape JSG
// gives that one: an object whose constructor nothing can construct and no
// global names, and whose one attribute, `compatibilityFlags`, is its own
// read-only property. src/tests/cloudflare-global/ runs one suite against both
// implementations, and its AGENTS.md records where they differ.
//
// main.ts defines the global as a lazy data property whose value is this
// module's `Cloudflare` export, so the module loads the first time the global
// is read. That is after user code may have run: it reads nothing through a
// prototype chain or a global that user code can replace.

import type { RuntimeClassHelpers } from './runtime-class';
import type { SerializationHelpers } from './serialization';

const {
  ErrorCaptureStackTrace,
  ObjectCreate,
  ObjectDefineProperty,
  ObjectFreeze,
  TypeError: TypeErrorCtor,
} = primordials;

const { markAsRuntimeClass } = require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;

const { compatibilityFlagStates } = utils;

// A function rather than a class, so that it has the shape JSG gives the C++
// constructor: a name and a length of 0, a `prototype` that is writable
// unless immutable_api_prototypes makes it read-only, and the same error
// called as constructed. The error's stack starts at the caller, as that of
// a native function's error does.
function Cloudflare(): void {
  const error = new TypeErrorCtor('Illegal constructor');
  ErrorCaptureStackTrace(error, Cloudflare);
  throw error;
}

// The prototype a function is created with: an object inheriting from
// Object.prototype whose `constructor` is the function, as JSG's is. It is
// captured here because user code can assign `Cloudflare.prototype`, though
// only once the global exists, since the global is the only way to the
// constructor.
const kPrototype = Cloudflare.prototype as object;

// What `Cloudflare.compatibilityFlags` is: an object without a prototype
// that maps the name of every compatibility enable-flag to whether it is
// set, in schema order, and that nothing can change. The properties are
// assigned, which on an object without a prototype runs no setter, then
// frozen.
function createCompatibilityFlags(): object {
  const flags = ObjectCreate(null) as Record<string, boolean>;
  const states = compatibilityFlagStates();
  for (let i = 0; i < states.length; i += 2) {
    flags[states[i] as string] = states[i + 1] as boolean;
  }
  return ObjectFreeze(flags);
}

// Makes the value of the `Cloudflare` global. `compatibilityFlags` is made
// with it: the C++ class makes it on its first read, which no code can tell
// from a property that already holds it.
function createCloudflare(): object {
  const cloudflare = ObjectCreate(kPrototype) as object;
  markUncloneable(cloudflare);
  ObjectDefineProperty(cloudflare, 'compatibilityFlags', {
    __proto__: null,
    value: createCompatibilityFlags(),
    writable: false,
    enumerable: true,
    configurable: true,
  } as PropertyDescriptor);
  return cloudflare;
}

if (compatFlags['immutable_api_prototypes'] === true) {
  ObjectDefineProperty(Cloudflare, 'prototype', {
    __proto__: null,
    writable: false,
  } as PropertyDescriptor);
}

markAsRuntimeClass(kPrototype, 'Cloudflare');

// The export that main.ts names: the value of the global.
module.exports = {
  Cloudflare: createCloudflare(),
};
