'use strict';

// The `scheduler` global, under the TS_SCHEDULER autogate. It replaces the C++
// Scheduler class in api/basics.h, with the shape JSG gives that one: an
// object whose constructor nothing can construct and no global names, and
// whose prototype holds wait().
//
// This module is the WebIDL layer of wait(): the receiver check, the argument
// count, and the conversion of the delay and of the WaitOptions dictionary.
// The wait itself is native, through utils.schedulerWait(): the timer, the
// signal, and so what the promise settles with and when are the same as in
// C++. src/tests/scheduler/ runs one suite against both implementations, and
// its AGENTS.md records where they differ.
//
// main.ts defines the global as a lazy data property whose value is this
// module's `scheduler` export, so the module loads the first time the global
// is read. That is after user code may have run: it reads nothing through a
// prototype chain or a global that user code can replace.

import type { RuntimeClassHelpers } from './runtime-class';
import type { SerializationHelpers } from './serialization';

const {
  ErrorCaptureStackTrace,
  ObjectCreate,
  ObjectDefineProperty,
  ObjectPrototype,
  PromiseReject,
  TypeError: TypeErrorCtor,
} = primordials;

const { markAsRuntimeClass, setNameAndLength } =
  require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;

const { schedulerWait } = utils;

// The C++ texts.
const ILLEGAL_INVOCATION =
  'Illegal invocation: function called with incorrect `this` reference. See ' +
  'https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const NO_DELAY =
  "Failed to execute 'wait' on 'Scheduler': " +
  "parameter 1 is not of type 'number'.";
const BAD_OPTIONS =
  "Failed to execute 'wait' on 'Scheduler': " +
  "parameter 2 is not of type 'WaitOptions'.";

// A function rather than a class, so that it has the shape JSG gives the C++
// constructor: a name and a length of 0, a `prototype` that is writable
// unless immutable_api_prototypes makes it read-only, and the same error
// called as constructed. The error's stack starts at the caller, as that of
// a native function's error does.
function Scheduler(): void {
  const error = new TypeErrorCtor('Illegal constructor');
  ErrorCaptureStackTrace(error, Scheduler);
  throw error;
}

// Whether a failure of wait() past its receiver check rejects its promise, as
// that of any JSG method returning a promise does under
// capture_async_api_throws, or is thrown.
const kCaptureThrows = compatFlags['capture_async_api_throws'] === true;

// The one Scheduler, which is the only receiver wait() takes: nothing can
// make another.
let scheduler: object | undefined;

function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

// A TypeError whose stack starts at the caller of wait(), as that of a native
// function's error does.
function newTypeError(message: string): TypeError {
  const error = new TypeErrorCtor(message);
  ErrorCaptureStackTrace(error, wait);
  return error;
}

// A method, so the function has no `prototype` and is not a constructor, like
// the JSG method it replaces.
const { wait } = {
  wait(this: unknown, delay?: unknown, options?: unknown): Promise<void> {
    if (this !== scheduler) {
      throw newTypeError(ILLEGAL_INVOCATION);
    }
    try {
      if (arguments.length === 0) {
        throw newTypeError(NO_DELAY);
      }
      const milliseconds = +(delay as number);
      let signal: unknown;
      if (options != null) {
        if (!isObject(options)) {
          throw newTypeError(BAD_OPTIONS);
        }
        signal = (options as { signal?: unknown }).signal;
      }
      return schedulerWait(milliseconds, signal);
    } catch (error) {
      if (!kCaptureThrows) {
        throw error;
      }
      return PromiseReject(error) as Promise<void>;
    }
  },
};
// JSG's length: the required arguments under
// spec_compliant_property_attributes, and 0 without.
setNameAndLength(
  wait,
  'wait',
  compatFlags['spec_compliant_property_attributes'] === true ? 1 : 0
);

// A prototype of our own rather than the one the function is created with,
// whose `constructor` would come before `wait`: JSG defines the members
// first.
const kPrototype = ObjectCreate(ObjectPrototype) as object;
ObjectDefineProperty(kPrototype, 'wait', {
  __proto__: null,
  value: wait,
  writable: true,
  enumerable: true,
  configurable: true,
} as PropertyDescriptor);
ObjectDefineProperty(kPrototype, 'constructor', {
  __proto__: null,
  value: Scheduler,
  writable: true,
  enumerable: false,
  configurable: true,
} as PropertyDescriptor);
ObjectDefineProperty(Scheduler, 'prototype', {
  __proto__: null,
  value: kPrototype,
  writable: compatFlags['immutable_api_prototypes'] !== true,
} as PropertyDescriptor);

markAsRuntimeClass(kPrototype, 'Scheduler');

scheduler = ObjectCreate(kPrototype) as object;
markUncloneable(scheduler);

// The export that main.ts names: the value of the global.
module.exports = { scheduler };
