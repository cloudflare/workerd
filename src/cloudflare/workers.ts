// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO(cleanup): C++ built-in modules do not yet support named exports, so we must define this
//   wrapper module that simply re-exports the classes from the built-in module.

import entrypoints from 'cloudflare-internal:workers';
import innerEnv from 'cloudflare-internal:env';
import { portMapper } from 'cloudflare-internal:http';

export const WorkerEntrypoint = entrypoints.WorkerEntrypoint;
export const DurableObject = entrypoints.DurableObject;
export const RpcStub = entrypoints.RpcStub;
export const RpcTarget = entrypoints.RpcTarget;
export const WorkflowEntrypoint = entrypoints.WorkflowEntrypoint;

export function withEnv(newEnv: unknown, fn: () => unknown): unknown {
  return innerEnv.withEnv(newEnv, fn);
}

// A proxy for the workers env/bindings. The proxy is io-context
// aware in that it will only return values when there is an active
// IoContext. Mutations to the env via this proxy propagate to the
// underlying env and do not follow the async context.
export const env = new Proxy(
  {},
  {
    get(_: unknown, prop: string | symbol): unknown {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.get(inner, prop);
      }
      return undefined;
    },

    set(_: unknown, prop: string | symbol, newValue: unknown): boolean {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.set(inner, prop, newValue);
      }
      return true;
    },

    has(_: unknown, prop: string | symbol): boolean {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.has(inner, prop);
      }
      return false;
    },

    ownKeys(_: unknown): ArrayLike<string | symbol> {
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.ownKeys(inner);
      }
      return [];
    },

    deleteProperty(_: unknown, prop: string | symbol): boolean {
      const inner = innerEnv.getCurrent();
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
      const inner = innerEnv.getCurrent();
      if (inner) {
        return Reflect.defineProperty(inner, prop, attr);
      }
      return true;
    },

    getOwnPropertyDescriptor(
      _: unknown,
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

export const waitUntil = entrypoints.waitUntil.bind(entrypoints);

export function nodeCompatHttpServerHandler(
  { port }: { port?: number } = {},
  handlers: Record<string, unknown> = {}
): {
  fetch(request: Request): Promise<Response>;
} {
  if (port == null) {
    throw new Error(
      'Port is required when calling nodeCompatHttpServerHandler()'
    );
  }
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (handlers == null || typeof handlers !== 'object') {
    throw new Error(
      'Handlers parameter passed to nodeCompatHttpServerHandler method must be an object'
    );
  }

  return Object.assign(handlers, {
    // We intentionally omitted ctx and env variables. Users should use
    // importable equivalents to access those values. For example, using
    // import { env, waitUntil } from 'cloudflare:workers
    async fetch(request: Request): Promise<Response> {
      const instance = portMapper.get(port);
      // TODO: Investigate supporting automatically assigned ports as being bound without a port configuration.
      if (!instance) {
        const error = new Error(
          `Http server with port ${port} not found. This is likely a bug with your code. You should check if server.listen() was called with the same port (${port})`
        );
        // @ts-expect-error TS2339 We're imitating Node.js errors.
        error.code = 'ERR_INVALID_ARG_VALUE';
        throw error;
      }
      return instance.fetch(request);
    },
  });
}
