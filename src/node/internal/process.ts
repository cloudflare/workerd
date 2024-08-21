// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Our implementation of process.nextTick is just queueMicrotask. The timing
// of when the queue is drained is different, as is the error handling so this
// is only an approximation of process.nextTick semantics. Hopefully it's good
// enough because we really do not want to implement Node.js' idea of a nextTick
// queue.
/* eslint-disable */

import { validateObject } from 'node-internal:validators';

import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors';

import { default as utilImpl } from 'node-internal:util';

export function nextTick(cb: Function, ...args: unknown[]) {
  queueMicrotask(() => {
    cb(...args);
  });
}

// Note that there is no process-level environment in workers so the process.env
// object is initially empty. This is different from Node.js where process.env
// picks up values from the operating system environment. The configured bindings
// for the worker are accessible from the env argument passed into the fetch
// handler and have no impact here.

export const env = new Proxy(
  {},
  {
    // Per Node.js rules. process.env values must be coerced to strings.
    // When defined using defineProperty, the property descriptor must be writable,
    // configurable, and enumerable using just a falsy check. Getters and setters
    // are not permitted.
    set(obj: object, prop: PropertyKey, value: any) {
      return Reflect.set(obj, prop, `${value}`);
    },
    defineProperty(
      obj: object,
      prop: PropertyKey,
      descriptor: PropertyDescriptor
    ) {
      validateObject(descriptor, 'descriptor', {});
      if (Reflect.has(descriptor, 'get') || Reflect.has(descriptor, 'set')) {
        throw new ERR_INVALID_ARG_VALUE(
          'descriptor',
          descriptor,
          'process.env value must not have getter/setter'
        );
      }
      if (!descriptor.configurable) {
        throw new ERR_INVALID_ARG_VALUE(
          'descriptor.configurable',
          descriptor,
          'process.env value must be configurable'
        );
      }
      if (!descriptor.enumerable) {
        throw new ERR_INVALID_ARG_VALUE(
          'descriptor.enumerable',
          descriptor,
          'process.env value must be enumerable'
        );
      }
      if (!descriptor.writable) {
        throw new ERR_INVALID_ARG_VALUE(
          'descriptor.writable',
          descriptor,
          'process.env value must be writable'
        );
      }
      if (Reflect.has(descriptor, 'value')) {
        Reflect.set(descriptor, 'value', `${descriptor.value}`);
      } else {
        throw new ERR_INVALID_ARG_VALUE(
          'descriptor.value',
          descriptor,
          'process.env value must be specified explicitly'
        );
      }
      return Reflect.defineProperty(obj, prop, descriptor);
    },
  }
);

export function getBuiltinModule(id: string): any {
  return utilImpl.getBuiltinModule(id);
}

export default {
  nextTick,
  env,
  getBuiltinModule,
};
