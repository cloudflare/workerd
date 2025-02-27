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
export const RpcTarget = entrypoints.RpcTarget;
export const WorkflowEntrypoint = entrypoints.WorkflowEntrypoint;

// We turn off these specific linting rules here intentionally as mixing
// typescript and proxies can just be... awkward. The use of any in these
// is intentional.
/* eslint-disable @typescript-eslint/no-unsafe-assignment */
/* eslint-disable @typescript-eslint/no-explicit-any */

export function withEnv(newEnv: unknown, fn: () => unknown): unknown {
  return innerEnv.withEnv(newEnv, fn);
}

// A proxy for the workers env/bindings. The proxy is io-context
// aware in that it will only return values when there is an active
// IoContext. Mutations to the env via this proxy propagate to the
// underlying env and do not follow the async context.
export const env: object = new Proxy(
  {},
  {
    get(_: any, prop: string | symbol): unknown {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.get(inner, prop);
      }
      return undefined;
    },

    set(_: any, prop: string | symbol, newValue: any): boolean {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.set(inner, prop, newValue);
      }
      return true;
    },

    has(_: any, prop: string | symbol): boolean {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.has(inner, prop);
      }
      return false;
    },

    ownKeys(_: any): any {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.ownKeys(inner);
      }
      return [];
    },

    deleteProperty(_: any, prop: string | symbol): boolean {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.deleteProperty(inner, prop);
      }
      return true;
    },

    defineProperty(
      _: any,
      prop: string | symbol,
      attr: PropertyDescriptor
    ): boolean {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.defineProperty(inner, prop, attr);
      }
      return true;
    },

    getOwnPropertyDescriptor(
      _: any,
      prop: string | symbol
    ): PropertyDescriptor | undefined {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.getOwnPropertyDescriptor(inner, prop);
      }
      return undefined;
    },
  }
);
