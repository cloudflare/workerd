// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT ORs
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';

import {
  validateBoolean,
  validateFunction,
  validateInteger,
  validateObject,
} from 'node-internal:validators';

const kEmptyObject = Object.create(null);
function kDefaultFunction() {}

// TODO(soon): MockTimers are currently still fairly experimental in Node.js.
// The intention is to implement them but for now, I'm skipping it.
//const { MockTimers } = require('internal/test_runner/mock/mock_timers');

export class MockFunctionContext {
  #calls;
  #mocks;
  #implementation;
  #restore;
  #times;

  constructor(implementation, restore, times) {
    this.#calls = [];
    this.#mocks = new Map();
    this.#implementation = implementation;
    this.#restore = restore;
    this.#times = times;
  }

  /**
   * Gets an array of recorded calls made to the mock function.
   * @returns {Array} An array of recorded calls.
   */
  get calls() {
    return this.#calls.slice(0);
  }

  /**
   * Retrieves the number of times the mock function has been called.
   * @returns {number} The call count.
   */
  callCount() {
    return this.#calls.length;
  }

  /**
   * Sets a new implementation for the mock function.
   * @param {Function} implementation - The new implementation for the mock function.
   */
  mockImplementation(implementation) {
    validateFunction(implementation, 'implementation');
    this.#implementation = implementation;
  }

  /**
   * Replaces the implementation of the function only once.
   * @param {Function} implementation - The substitute function.
   * @param {number} [onCall] - The call index to be replaced.
   */
  mockImplementationOnce(implementation, onCall) {
    validateFunction(implementation, 'implementation');
    const nextCall = this.#calls.length;
    const call = onCall ?? nextCall;
    validateInteger(call, 'onCall', nextCall);
    this.#mocks.set(call, implementation);
  }

  /**
   * Restores the original function that was mocked.
   */
  restore() {
    const { descriptor, object, original, methodName } = this.#restore;

    if (typeof methodName === 'string') {
      // This is an object method spy.
      Object.defineProperty(object, methodName, descriptor);
    } else {
      // This is a bare function spy. There isn't much to do here but make
      // the mock call the original function.
      this.#implementation = original;
    }
  }

  /**
   * Resets the recorded calls to the mock function
   */
  resetCalls() {
    this.#calls = [];
  }

  /**
   * Tracks a call made to the mock function.
   * @param {object} call - The call details.
   */
  trackCall(call) {
    this.#calls.push(call);
  }

  /**
   * Gets the next implementation to use for the mock function.
   * @returns {Function} The next implementation.
   */
  nextImpl() {
    const nextCall = this.#calls.length;
    const mock = this.#mocks.get(nextCall);
    const impl = mock ?? this.#implementation;

    if (nextCall + 1 === this.#times) {
      this.restore();
    }

    this.#mocks.delete(nextCall);
    return impl;
  }
}

const { nextImpl, restore, trackCall } = MockFunctionContext.prototype;
delete MockFunctionContext.prototype.trackCall;
delete MockFunctionContext.prototype.nextImpl;

export class MockTracker {
  #mocks = [];

  // TODO(soon): MockTimers are currently still fairly experimental in Node.js.
  // The intention is to implement them but for now, I'm skipping it.
  // #timers;

  // /**
  //  * Returns the mock timers of this MockTracker instance.
  //  * @returns {MockTimers} The mock timers instance.
  //  */
  // get timers() {
  //   this.#timers ??= new MockTimers();
  //   return this.#timers;
  // }

  /**
   * Creates a mock function tracker.
   * @param {Function} [original] - The original function to be tracked.
   * @param {Function} [implementation] - An optional replacement function for the original one.
   * @param {object} [options] - Additional tracking options.
   * @param {number} [options.times=Infinity] - The maximum number of times the mock function can be called.
   * @returns {ProxyConstructor} The mock function tracker.
   */
  fn(
    original = function() {},
    implementation = original,
    options = kEmptyObject,
  ) {
    if (original !== null && typeof original === 'object') {
      options = original;
      original = function() {};
      implementation = original;
    } else if (implementation !== null && typeof implementation === 'object') {
      options = implementation;
      implementation = original;
    }

    validateFunction(original, 'original');
    validateFunction(implementation, 'implementation');
    validateObject(options, 'options');
    const { times = Infinity } = options;
    validateTimes(times, 'options.times');
    const ctx = new MockFunctionContext(implementation, { __proto__: null, original }, times);
    return this.#setupMock(ctx, original);
  }

  /**
   * Creates a method tracker for a specified object or function.
   * @param {(object | Function)} objectOrFunction - The object or function containing the method to be tracked.
   * @param {string} methodName - The name of the method to be tracked.
   * @param {Function} [implementation] - An optional replacement function for the original method.
   * @param {object} [options] - Additional tracking options.
   * @param {boolean} [options.getter=false] - Indicates whether this is a getter method.
   * @param {boolean} [options.setter=false] - Indicates whether this is a setter method.
   * @param {number} [options.times=Infinity] - The maximum number of times the mock method can be called.
   * @returns {ProxyConstructor} The mock method tracker.
   */
  method(
    objectOrFunction,
    methodName,
    implementation = kDefaultFunction,
    options = kEmptyObject,
  ) {
    validateStringOrSymbol(methodName, 'methodName');
    if (typeof objectOrFunction !== 'function') {
      validateObject(objectOrFunction, 'object');
    }

    if (implementation !== null && typeof implementation === 'object') {
      options = implementation;
      implementation = kDefaultFunction;
    }

    validateFunction(implementation, 'implementation');
    validateObject(options, 'options');

    const {
      getter = false,
      setter = false,
      times = Infinity,
    } = options;

    validateBoolean(getter, 'options.getter');
    validateBoolean(setter, 'options.setter');
    validateTimes(times, 'options.times');

    if (setter && getter) {
      throw new ERR_INVALID_ARG_VALUE(
        'options.setter', setter, "cannot be used with 'options.getter'",
      );
    }
    const descriptor = findMethodOnPrototypeChain(objectOrFunction, methodName);

    let original;

    if (getter) {
      original = descriptor?.get;
    } else if (setter) {
      original = descriptor?.set;
    } else {
      original = descriptor?.value;
    }

    if (typeof original !== 'function') {
      throw new ERR_INVALID_ARG_VALUE(
        'methodName', original, 'must be a method',
      );
    }

    const restore = { __proto__: null, descriptor, object: objectOrFunction, methodName };
    const impl = implementation === kDefaultFunction ?
      original : implementation;
    const ctx = new MockFunctionContext(impl, restore, times);
    const mock = this.#setupMock(ctx, original);
    const mockDescriptor = {
      __proto__: null,
      configurable: descriptor.configurable,
      enumerable: descriptor.enumerable,
    };

    if (getter) {
      mockDescriptor.get = mock;
      mockDescriptor.set = descriptor.set;
    } else if (setter) {
      mockDescriptor.get = descriptor.get;
      mockDescriptor.set = mock;
    } else {
      mockDescriptor.writable = descriptor.writable;
      mockDescriptor.value = mock;
    }

    Object.defineProperty(objectOrFunction, methodName, mockDescriptor);

    return mock;
  }

  /**
   * Mocks a getter method of an object.
   * This is a syntax sugar for the MockTracker.method with options.getter set to true
   * @param {object} object - The target object.
   * @param {string} methodName - The name of the getter method to be mocked.
   * @param {Function} [implementation] - An optional replacement function for the targeted method.
   * @param {object} [options] - Additional tracking options.
   * @param {boolean} [options.getter=true] - Indicates whether this is a getter method.
   * @param {boolean} [options.setter=false] - Indicates whether this is a setter method.
   * @param {number} [options.times=Infinity] - The maximum number of times the mock method can be called.
   * @returns {ProxyConstructor} The mock method tracker.
   */
  getter(
    object,
    methodName,
    implementation = kDefaultFunction,
    options = kEmptyObject,
  ) {
    if (implementation !== null && typeof implementation === 'object') {
      options = implementation;
      implementation = kDefaultFunction;
    } else {
      validateObject(options, 'options');
    }

    const { getter = true } = options;

    if (getter === false) {
      throw new ERR_INVALID_ARG_VALUE(
        'options.getter', getter, 'cannot be false',
      );
    }

    return this.method(object, methodName, implementation, {
      __proto__: null,
      ...options,
      getter,
    });
  }

  /**
   * Mocks a setter method of an object.
   * This function is a syntax sugar for MockTracker.method with options.setter set to true.
   * @param {object} object - The target object.
   * @param {string} methodName  - The setter method to be mocked.
   * @param {Function} [implementation] - An optional replacement function for the targeted method.
   * @param {object} [options] - Additional tracking options.
   * @param {boolean} [options.getter=false] - Indicates whether this is a getter method.
   * @param {boolean} [options.setter=true] - Indicates whether this is a setter method.
   * @param {number} [options.times=Infinity] - The maximum number of times the mock method can be called.
   * @returns {ProxyConstructor} The mock method tracker.
   */
  setter(
    object,
    methodName,
    implementation = kDefaultFunction,
    options = kEmptyObject,
  ) {
    if (implementation !== null && typeof implementation === 'object') {
      options = implementation;
      implementation = kDefaultFunction;
    } else {
      validateObject(options, 'options');
    }

    const { setter = true } = options;

    if (setter === false) {
      throw new ERR_INVALID_ARG_VALUE(
        'options.setter', setter, 'cannot be false',
      );
    }

    return this.method(object, methodName, implementation, {
      __proto__: null,
      ...options,
      setter,
    });
  }

  /**
   * Resets the mock tracker, restoring all mocks and clearing timers.
   */
  reset() {
    this.restoreAll();
    // this.#timers?.reset();
    this.#mocks = [];
  }

  /**
   * Restore all mocks created by this MockTracker instance.
   */
  restoreAll() {
    for (let i = 0; i < this.#mocks.length; i++) {
      restore.call(this.#mocks[i]);
    }
  }

  #setupMock(ctx, fnToMatch) {
    const mock = new Proxy(fnToMatch, {
      __proto__: null,
      apply(_fn, thisArg, argList) {
        const fn = nextImpl.call(ctx);
        let result;
        let error;

        try {
          result = Reflect.apply(fn, thisArg, argList);
        } catch (err) {
          error = err;
          throw err;
        } finally {
          trackCall.call(ctx, {
            __proto__: null,
            arguments: argList,
            error,
            result,
            // eslint-disable-next-line no-restricted-syntax
            stack: new Error(),
            target: undefined,
            this: thisArg,
          });
        }

        return result;
      },
      construct(target, argList, newTarget) {
        const realTarget = nextImpl.call(ctx);
        let result;
        let error;

        try {
          result = Reflect.construct(realTarget, argList, newTarget);
        } catch (err) {
          error = err;
          throw err;
        } finally {
          trackCall.call(ctx, {
            __proto__: null,
            arguments: argList,
            error,
            result,
            // eslint-disable-next-line no-restricted-syntax
            stack: new Error(),
            target,
            this: result,
          });
        }

        return result;
      },
      get(target, property, receiver) {
        if (property === 'mock') {
          return ctx;
        }

        return Reflect.get(target, property, receiver);
      },
    });

    this.#mocks.push(ctx);
    return mock;
  }
}

function validateStringOrSymbol(value, name) {
  if (typeof value !== 'string' && typeof value !== 'symbol') {
    throw new ERR_INVALID_ARG_TYPE(name, ['string', 'symbol'], value);
  }
}

function validateTimes(value, name) {
  if (value === Infinity) {
    return;
  }

  validateInteger(value, name, 1);
}

function findMethodOnPrototypeChain(instance, methodName) {
  let host = instance;
  let descriptor;

  while (host !== null) {
    descriptor = Object.getOwnPropertyDescriptor(host, methodName);

    if (descriptor) {
      break;
    }

    host = Object.getPrototypeOf(host);
  }

  return descriptor;
}
