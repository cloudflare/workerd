'use strict';

// The `navigator` global and its Navigator class, under the TS_NAVIGATOR
// autogate. They replace the C++ Navigator in api/global-scope.h, with the
// shape JSG gives it: a constructor nothing can construct, sendBeacon() on
// the prototype, and every attribute an own property of the instance.
// src/tests/navigator/ runs one suite against both implementations, and its
// AGENTS.md records where they differ.
//
// main.ts installs the constructor. The global itself stays the lazy property
// that global-scope.h registers, whose getter calls createNavigator() the
// first time `navigator` is read.
//
// What needs the request, JSG's conversion of a request body or a C++ object
// is behind NavigatorNative (api/global-scope.h): sendBeacon()'s fetch, and
// the StorageManager that `storage` is.

import type { RuntimeClassHelpers } from './runtime-class';
import type { SerializationHelpers } from './serialization';

const {
  ErrorCaptureStackTrace,
  ObjectCreate,
  ObjectDefineProperty,
  ObjectSetPrototypeOf,
  TypeError: TypeErrorCtor,
} = primordials;

const { markAsRuntimeClass, setNameAndLength } =
  require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;

const { createNavigatorNative, defineComputedProperty, getPrivateSymbol } =
  utils;

// V8-private, so that it is neither inherited nor forgeable: an object that
// inherits from the navigator is not one.
const kBrand = getPrivateSymbol('workerd:Navigator');

const kHasLanguage = compatFlags['enable_navigator_language'] === true;
const kHasStorage = compatFlags['enable_web_file_system'] === true;

const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const kMissingUrl =
  "Failed to execute 'sendBeacon' on 'Navigator': parameter 1 is not of " +
  "type 'string'.";
const kSymbolArgument = 'Cannot convert a Symbol value to a string';

type Branded = Record<symbol, unknown>;

// A TypeError whose stack starts at the code that called `fn`, the function
// throwing it, as that of a native function's error does.
function typeError(message: string, fn: unknown): TypeError {
  const error = new TypeErrorCtor(message);
  ErrorCaptureStackTrace(error, fn as (...args: never[]) => unknown);
  return error;
}

function isNavigator(value: unknown): boolean {
  return (
    typeof value === 'object' &&
    value !== null &&
    (value as Branded)[kBrand] === true
  );
}

// Made on first use, since every isolate runs this module and few send a
// beacon or read `storage`.
let native: NavigatorNative | undefined;

function getNative(): NavigatorNative {
  return (native ??= createNavigatorNative());
}

// The constructor is a function rather than a class so that it has the shape
// JSG gives the C++ one: its `prototype` is writable unless
// immutable_api_prototypes makes it read-only, and it throws the same error
// called as constructed.
function Navigator(): void {
  throw typeError('Illegal constructor', Navigator);
}

// The prototype's members. A method defined in an object literal is
// enumerable, as JSG's are, and is not a constructor. The object is the
// prototype itself, so that its `constructor`, defined below, follows the
// method as JSG's does. The method names itself as `sendBeacon`, captured
// below: user code can assign to the prototype's property.
const kPrototype = {
  sendBeacon(this: unknown, url?: unknown, body?: unknown): boolean {
    // In JSG's order: the receiver, the argument count, the URL's
    // conversion, then the body's, which NavigatorNative makes.
    if (!isNavigator(this)) {
      throw typeError(kIllegalInvocation, sendBeacon);
    }
    if (arguments.length === 0) {
      throw typeError(kMissingUrl, sendBeacon);
    }
    // ToString() throws for a Symbol itself, with a stack that begins here.
    if (typeof url === 'symbol') {
      throw typeError(kSymbolArgument, sendBeacon);
    }
    const urlString = typeof url === 'string' ? url : `${url as string}`;
    if (typeof body === 'symbol') {
      throw typeError(kSymbolArgument, sendBeacon);
    }
    return getNative().sendBeacon(urlString, body);
  },
};

const { sendBeacon } = kPrototype;

// A new array on every read, as the C++ getter returns.
function getLanguages(): string[] {
  return ['en'];
}

// The descriptor of an attribute, reused for every one. Its prototype is
// null, so that defining with it reads nothing user code can patch.
class AttributeDescriptor {
  declare value: unknown;
  declare writable: boolean;
  declare enumerable: boolean;
  declare configurable: boolean;
  constructor() {
    this.value = undefined;
    this.writable = false;
    this.enumerable = true;
    this.configurable = true;
  }
}
ObjectSetPrototypeOf(AttributeDescriptor.prototype, null);
const kAttribute = new AttributeDescriptor();

function defineAttribute(self: object, name: string, value: unknown): void {
  kAttribute.value = value;
  ObjectDefineProperty(self, name, kAttribute as PropertyDescriptor);
  kAttribute.value = undefined;
}

// A new Navigator. The attributes are its own read-only properties, in the
// order V8 gives those of a C++ instance: the reverse of the order in which
// global-scope.h registers them.
function createNavigator(): object {
  const navigator = ObjectCreate(kPrototype) as object;
  (navigator as Branded)[kBrand] = true;
  markUncloneable(navigator);
  if (kHasStorage) {
    defineAttribute(navigator, 'storage', getNative().createStorage());
  }
  if (kHasLanguage) {
    defineComputedProperty(navigator, 'languages', getLanguages);
    defineAttribute(navigator, 'language', 'en');
  }
  defineAttribute(navigator, 'platform', '');
  // Workers does not expose hardware concurrency: to user code there is one
  // core.
  defineAttribute(navigator, 'hardwareConcurrency', 1);
  defineAttribute(navigator, 'userAgent', 'Cloudflare-Workers');
  return navigator;
}

// ---------------------------------------------------------------------------
// The rest gives the constructor the shape JSG gives the C++ one.

ObjectDefineProperty(kPrototype, 'constructor', {
  __proto__: null,
  value: Navigator,
  writable: true,
  enumerable: false,
  configurable: true,
} as PropertyDescriptor);
// Still writable here, whatever the flags. createNavigator() uses kPrototype,
// not what user code assigns to Navigator.prototype later.
Navigator.prototype = kPrototype;
if (compatFlags['immutable_api_prototypes'] === true) {
  ObjectDefineProperty(Navigator, 'prototype', {
    __proto__: null,
    writable: false,
  } as PropertyDescriptor);
}

// JSG counts the required argument only under
// spec_compliant_property_attributes.
setNameAndLength(
  sendBeacon,
  'sendBeacon',
  compatFlags['spec_compliant_property_attributes'] === true ? 1 : 0
);
setNameAndLength(Navigator, 'Navigator', 0);
markAsRuntimeClass(kPrototype, 'Navigator');

export type NavigatorExports = {
  Navigator: typeof Navigator;
  createNavigator: typeof createNavigator;
};

module.exports = {
  Navigator,
  createNavigator,
};
