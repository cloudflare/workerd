// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO(cleanup): C++ built-in modules do not yet support named exports, so we must define this
//   wrapper module that simply re-exports the classes from the built-in module.

import entrypoints from 'cloudflare-internal:workers';
import innerEnv from 'cloudflare-internal:env';

export const WorkerEntrypoint = entrypoints.WorkerEntrypoint;
export const DurableObject = entrypoints.DurableObject;
export const RpcStub = entrypoints.RpcStub;
export const RpcPromise = entrypoints.RpcPromise;
export const RpcProperty = entrypoints.RpcProperty;
export const RpcTarget = entrypoints.RpcTarget;
export const ServiceStub = entrypoints.ServiceStub;
export const WorkflowEntrypoint = entrypoints.WorkflowEntrypoint;

export function withEnv(newEnv: unknown, fn: () => unknown): unknown {
  return innerEnv.withEnv(newEnv, fn);
}

export function withExports(newExports: unknown, fn: () => unknown): unknown {
  return innerEnv.withExports(newExports, fn);
}

export function withEnvAndExports(
  newEnv: unknown,
  newExports: unknown,
  fn: () => unknown
): unknown {
  return innerEnv.withEnvAndExports(newEnv, newExports, fn);
}

// A proxy for the workers env/bindings. Since env is imported as a module-level
// reference, the object identity cannot be changed. The proxy provides indirection,
// delegating to different underlying env objects based on async context (see withEnv()).
// Mutations via this proxy modify the current underlying env object in-place - if you're
// inside a withEnv() scope, mutations affect the override object, not the base environment.
export const env = new Proxy(
  {},
  {
    get(_: unknown, prop: string | symbol): unknown {
      const inner = innerEnv.getCurrentEnv();
      if (inner) {
        return Reflect.get(inner, prop);
      }
      return undefined;
    },

    set(_: unknown, prop: string | symbol, newValue: unknown): boolean {
      const inner = innerEnv.getCurrentEnv();
      if (inner) {
        return Reflect.set(inner, prop, newValue);
      }
      return true;
    },

    has(_: unknown, prop: string | symbol): boolean {
      const inner = innerEnv.getCurrentEnv();
      if (inner) {
        return Reflect.has(inner, prop);
      }
      return false;
    },

    ownKeys(_: unknown): ArrayLike<string | symbol> {
      const inner = innerEnv.getCurrentEnv();
      if (inner) {
        return Reflect.ownKeys(inner);
      }
      return [];
    },

    deleteProperty(_: unknown, prop: string | symbol): boolean {
      const inner = innerEnv.getCurrentEnv();
      if (inner) {
        return Reflect.deleteProperty(inner, prop);
      }
      return true;
    },

    defineProperty(
      _: unknown,
      prop: string | symbol,
      attr: PropertyDescriptor
    ): boolean {
      const inner = innerEnv.getCurrentEnv();
      if (inner) {
        return Reflect.defineProperty(inner, prop, attr);
      }
      return true;
    },

    getOwnPropertyDescriptor(
      _: unknown,
      prop: string | symbol
    ): PropertyDescriptor | undefined {
      const inner = innerEnv.getCurrentEnv();
      if (inner) {
        return Reflect.getOwnPropertyDescriptor(inner, prop);
      }
      return undefined;
    },
  }
);

// A proxy for the worker exports. Since exports is imported as a module-level
// reference, the object identity cannot be changed. The proxy provides indirection,
// delegating to different underlying exports objects based on async context (see
// withExports()). This proxy is read-only - mutations are not supported.
export const exports = new Proxy(
  {},
  {
    get(_: unknown, prop: string | symbol): unknown {
      const inner = innerEnv.getCurrentExports();
      if (inner) {
        return Reflect.get(inner, prop);
      }
      return undefined;
    },

    has(_: unknown, prop: string | symbol): boolean {
      const inner = innerEnv.getCurrentExports();
      if (inner) {
        return Reflect.has(inner, prop);
      }
      return false;
    },

    ownKeys(_: unknown): ArrayLike<string | symbol> {
      const inner = innerEnv.getCurrentExports();
      if (inner) {
        return Reflect.ownKeys(inner);
      }
      return [];
    },

    getOwnPropertyDescriptor(
      _: unknown,
      prop: string | symbol
    ): PropertyDescriptor | undefined {
      const inner = innerEnv.getCurrentExports();
      if (inner) {
        return Reflect.getOwnPropertyDescriptor(inner, prop);
      }
      return undefined;
    },
  }
);

export const waitUntil = entrypoints.waitUntil.bind(entrypoints);
