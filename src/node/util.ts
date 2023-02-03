// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as internalTypes } from 'node-internal:internal_types';

import {
  validateFunction
} from 'node-internal:validators';

import {
  ERR_FALSY_VALUE_REJECTION
} from 'node-internal:internal_errors';

export const types = internalTypes;

const callbackifyOnRejected = (reason: unknown, cb : Function) => {
  if (!reason) {
    reason = new ERR_FALSY_VALUE_REJECTION(`${reason}`);
  }
  return cb(reason);
};

export function callbackify
    <T extends (...args: any[]) => Promise<any>>(original: T):
    T extends (...args: infer TArgs) => Promise<infer TReturn> ? (...params: [...TArgs, (err: Error, ret: TReturn) => any]) => void : never {
  validateFunction(original, 'original');

  function callbackified(this: unknown,
                         ...args: [...unknown[],
                         (err: unknown, ret: unknown) => void]) : any {
    const maybeCb = args.pop();
    validateFunction(maybeCb, 'last argument');
    const cb = (maybeCb as Function).bind(this);
    Reflect.apply(original, this, args)
      .then((ret: any) => queueMicrotask(() => cb(null, ret)),
            (rej: any) => queueMicrotask(() => callbackifyOnRejected(rej, cb)));
  }

  const descriptors = Object.getOwnPropertyDescriptors(original);
  if (typeof descriptors['length']!.value === 'number') {
    descriptors['length']!.value++;
  }
  if (typeof descriptors['name']!.value === 'string') {
    descriptors['name']!.value += 'Callbackified';
  }
  const propertiesValues = Object.values(descriptors);
  for (let i = 0; i < propertiesValues.length; i++) {
    Object.setPrototypeOf(propertiesValues[i], null);
  }
  Object.defineProperties(callbackified, descriptors);
  return callbackified as any;
}

const kCustomPromisifiedSymbol = Symbol.for('nodejs.util.promisify.custom');
const kCustomPromisifyArgsSymbol = Symbol('customPromisifyArgs');

// TODO(later): Proper type signature for promisify.
export function promisify(original: Function): Function {
  validateFunction(original, 'original');

  if ((original as any)[kCustomPromisifiedSymbol]) {
    const fn = (original as any)[kCustomPromisifiedSymbol];

    validateFunction(fn, 'util.promisify.custom');

    return Object.defineProperty(fn, kCustomPromisifiedSymbol, {
      value: fn,
      enumerable: false,
      writable: false,
      configurable: true
    });
  }

  // Names to create an object from in case the callback receives multiple
  // arguments, e.g. ['bytesRead', 'buffer'] for fs.read.
  const argumentNames = (original as any)[kCustomPromisifyArgsSymbol];

  function fn(this: any, ...args: any[]) {
    return new Promise((resolve, reject) => {
      args.push((err: unknown, ...values: unknown[]) => {
        if (err) {
          return reject(err);
        }
        if (argumentNames !== undefined && values.length > 1) {
          const obj = {};
          for (let i = 0; i < argumentNames.length; i++)
            (obj as any)[argumentNames[i]] = values[i];
          resolve(obj);
        } else {
          resolve(values[0]);
        }
      });
      Reflect.apply(original, this, args);
    });
  }

  Object.setPrototypeOf(fn, Object.getPrototypeOf(original));

  Object.defineProperty(fn, kCustomPromisifiedSymbol, {
    value: fn,
    enumerable: false,
    writable: false,
    configurable: true
  });

  const descriptors = Object.getOwnPropertyDescriptors(original);
  const propertiesValues = Object.values(descriptors);
  for (let i = 0; i < propertiesValues.length; i++) {
    // We want to use null-prototype objects to not rely on globally mutable
    // %Object.prototype%.
    Object.setPrototypeOf(propertiesValues[i], null);
  }
  return Object.defineProperties(fn, descriptors);
}

promisify.custom = kCustomPromisifiedSymbol;

export default {
  types,
  callbackify,
  promisify,
};
