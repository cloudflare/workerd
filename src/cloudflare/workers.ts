// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO(cleanup): C++ built-in modules do not yet support named exports, so we must define this
//   wrapper module that simply re-exports the classes from the built-in module.

import entrypoints from 'cloudflare-internal:workers';
import innerEnv from 'cloudflare-internal:env';
import innerTracing from 'cloudflare-internal:tracing';

export const WorkerEntrypoint = entrypoints.WorkerEntrypoint;
export const DurableObject = entrypoints.DurableObject;
export const RpcStub = entrypoints.RpcStub;
export const RpcPromise = entrypoints.RpcPromise;
export const RpcProperty = entrypoints.RpcProperty;
export const RpcTarget = entrypoints.RpcTarget;
export const ServiceStub = entrypoints.ServiceStub;

type RollbackFn = ((...args: unknown[]) => Promise<void>) | null;
type RollbackConfig = Record<string, unknown> | null;
type ExecuteFn = (
  rollbackFn: RollbackFn,
  rollbackConfig: RollbackConfig
) => Promise<unknown>;
type RunFn = (event: unknown, step: unknown, ...rest: unknown[]) => unknown;

interface StepRpcStub {
  do(...args: unknown[]): Promise<unknown>;
  waitForEvent(...args: unknown[]): Promise<unknown>;
  [method: string]: (...args: unknown[]) => unknown;
}

// StepPromise is a real Promise subclass that captures .rollback() synchronously before
// firing the actual RPC. This is necessary because step.do() returns an RPC promise --
// accessing .rollback() on it after resolution would be interpreted as a pipelined RPC call
// on the resolved value. By deferring execution until .then(), we can bundle the callback
// and rollback function in a single RPC call.
//
// We extend Promise so that instanceof Promise is true and the d.ts declaration
// (StepPromise<T> extends Promise<T>) is accurate at runtime.
class StepPromise extends Promise<unknown> {
  #execute: ExecuteFn | null;
  #rollbackFn: RollbackFn = null;
  #rollbackConfig: RollbackConfig = null;
  #hasRollback = false;
  #promise: Promise<unknown> | null = null;
  #launched = false;

  // Tell the engine to construct plain Promises (not StepPromise) for internal
  // operations like finally(). Without this, finally() would try to construct a
  // StepPromise using the standard (resolve, reject) executor contract, which
  // our constructor does not implement.
  static override get [Symbol.species](): PromiseConstructor {
    return Promise;
  }

  constructor(execute: ExecuteFn) {
    // No-op executor — the real work is deferred until .then() is called.
    // This is required by the Promise constructor but we never use the
    // resolve/reject it provides; instead we delegate to #getPromise().
    super(() => {});
    this.#execute = execute;
  }

  rollback(
    configOrFn: RollbackConfig | RollbackFn,
    maybeFn?: RollbackFn
  ): this {
    if (this.#launched) {
      throw new Error('.rollback() must be called before the step is awaited');
    }
    if (this.#hasRollback) {
      throw new Error('.rollback() can only be called once per step');
    }
    this.#hasRollback = true;

    if (typeof configOrFn === 'function') {
      // rollback(fn)
      this.#rollbackFn = configOrFn;
    } else if (configOrFn != null && typeof configOrFn === 'object') {
      // rollback(config, fn)
      if (typeof maybeFn !== 'function') {
        throw new TypeError(
          '.rollback(config, fn) requires the second argument to be a function'
        );
      }
      this.#rollbackConfig = configOrFn;
      this.#rollbackFn = maybeFn;
    } else {
      throw new TypeError(
        '.rollback() expects a function, or a config object followed by a function'
      );
    }

    return this;
  }

  #getPromise(): Promise<unknown> {
    if (!this.#promise) {
      this.#launched = true;
      const execute = this.#execute;
      if (execute === null) {
        throw new Error('StepPromise execute function is missing');
      }
      this.#promise = execute(this.#rollbackFn, this.#rollbackConfig);
      // Allow GC of the closure and rollback references now that the RPC has been dispatched.
      this.#execute = null;
      this.#rollbackFn = null;
      this.#rollbackConfig = null;
    }
    return this.#promise;
  }

  // Override then() to delegate to the deferred promise rather than the no-op
  // super promise. This is what makes the lazy execution work. catch() delegates
  // through then() per the ES spec; finally() uses Symbol.species (set above)
  // to construct a plain Promise instead of a StepPromise.
  //
  // The generic signature matches Promise.prototype.then from lib.es5.d.ts —
  // TypeScript requires it for a compatible override.
  override then<TResult1 = unknown, TResult2 = never>(
    onFulfilled?: ((value: unknown) => TResult1 | PromiseLike<TResult1>) | null,
    onRejected?: ((reason: unknown) => TResult2 | PromiseLike<TResult2>) | null
  ): Promise<TResult1 | TResult2> {
    return this.#getPromise().then(onFulfilled, onRejected);
  }
}

// Wraps the step RPC stub so that step.do() and step.waitForEvent() return StepPromise instances.
// All other methods (sleep, sleepUntil, and any future additions) are forwarded unchanged via Proxy.
function wrapStep(jsStep: StepRpcStub): StepRpcStub {
  return new Proxy(jsStep, {
    get(
      target: StepRpcStub,
      prop: string | symbol,
      receiver: unknown
    ): unknown {
      if (prop === 'do') {
        return (
          name: string,
          configOrCallback: unknown,
          maybeCallback?: unknown
        ): StepPromise => {
          return new StepPromise(
            (rollbackFn: RollbackFn, rollbackConfig: RollbackConfig) => {
              const args: unknown[] = [name];
              if (maybeCallback !== undefined) {
                args.push(configOrCallback, maybeCallback);
              } else {
                args.push(configOrCallback);
              }
              if (rollbackFn !== null) {
                args.push(rollbackFn);
                if (rollbackConfig !== null) {
                  args.push(rollbackConfig);
                }
              }
              return target.do(...args);
            }
          );
        };
      }
      if (prop === 'waitForEvent') {
        return (name: string, options: unknown): StepPromise => {
          return new StepPromise(
            (rollbackFn: RollbackFn, rollbackConfig: RollbackConfig) => {
              const args: unknown[] = [name, options];
              if (rollbackFn !== null) {
                args.push(rollbackFn);
                if (rollbackConfig !== null) {
                  args.push(rollbackConfig);
                }
              }
              return target.waitForEvent(...args);
            }
          );
        };
      }
      return Reflect.get(target, prop, receiver) as unknown;
    },
  });
}

// Wraps a run function so that its second argument (step) is replaced with wrapStep(step).
function makeWrappedRun(originalRun: RunFn): RunFn {
  return function (
    this: unknown,
    event: unknown,
    step: unknown,
    ...rest: unknown[]
  ): unknown {
    return originalRun.call(
      this,
      event,
      wrapStep(step as StepRpcStub),
      ...rest
    );
  };
}

// Wrap WorkflowEntrypoint to intercept run() calls and wrap the step argument before it reaches
// user code. This provides an extension point for step-level features (rollback, future additions)
// without modifying the C++ entrypoint.
//
// We use a JS subclass (not a Proxy) because the runtime walks the constructor prototype chain
// to classify entrypoints (workflowClasses vs actorClasses vs statelessClasses). A Proxy breaks
// identity comparison: `Proxy !== target`. A JS subclass preserves the chain:
//   UserWorkflow -> our JS class -> C++ WorkflowEntrypoint (matched by runtime)
// Tracks which prototypes have already had their run() wrapped, so we don't
// double-wrap on the second instantiation of the same class.
const wrappedProtos = new WeakSet<object>();

class WorkflowEntrypointWrapper extends entrypoints.WorkflowEntrypoint {
  constructor(ctx: unknown, env: unknown) {
    super(ctx, env);

    // Walk the prototype chain to find the prototype that owns run() and wrap it.
    // We stop at WorkflowEntrypointWrapper.prototype to avoid patching the C++ base.
    // This handles inheritance: class B extends A extends WorkflowEntrypoint where
    // only A defines run() — getPrototypeOf(b) is B.prototype which doesn't own run,
    // so we walk up to A.prototype which does.
    let proto: Record<string, unknown> | null = Object.getPrototypeOf(
      this
    ) as Record<string, unknown>;
    const stop = WorkflowEntrypointWrapper.prototype as unknown;
    while (proto !== null && proto !== stop) {
      if (
        !wrappedProtos.has(proto) &&
        Object.prototype.hasOwnProperty.call(proto, 'run') &&
        typeof proto.run === 'function'
      ) {
        const originalRun = proto.run as RunFn;
        proto.run = makeWrappedRun(originalRun);
        wrappedProtos.add(proto);
        break;
      }
      proto = Object.getPrototypeOf(proto) as Record<string, unknown> | null;
    }

    // NOTE: Arrow function class properties (e.g. `run = async () => {}`) are NOT supported.
    // They define `run` as an instance property, not a prototype method. The C++ RPC dispatch
    // resolves methods via the prototype chain, so arrow function `run` is invisible to RPC.
    // This is a workerd-level constraint that applies to all entrypoints (WorkerEntrypoint,
    // DurableObject, WorkflowEntrypoint).
  }
}

export const WorkflowEntrypoint = Cloudflare.compatibilityFlags
  .workflows_step_rollback
  ? WorkflowEntrypointWrapper
  : entrypoints.WorkflowEntrypoint;

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

// A proxy for the worker's cache context (ctx.cache). Since cache is imported as a module-level
// reference, the object identity cannot be changed. The proxy provides indirection, delegating
// to the current request's CacheContext. This ensures that cache remains entrypoint specific
// ensuring that the runtime always delegates the right host to clear.
export const cache = new Proxy(
  {},
  {
    get(_: unknown, prop: string | symbol): unknown {
      const inner = entrypoints.getCtxCache();
      if (inner) {
        const value: unknown = Reflect.get(inner, prop);
        // Bind methods to the underlying CacheContext so that `this` is correct
        // when calling e.g. cache.purge() through the proxy.
        if (typeof value === 'function') {
          return Function.prototype.bind.call(value, inner);
        }
        return value;
      }
      // Used to enable safe no-op access outside module init.
      return undefined;
    },

    has(_: unknown, prop: string | symbol): boolean {
      const inner = entrypoints.getCtxCache();
      if (inner) {
        return Reflect.has(inner, prop);
      }
      return false;
    },

    ownKeys(_: unknown): ArrayLike<string | symbol> {
      const inner = entrypoints.getCtxCache();
      if (inner) {
        return Reflect.ownKeys(inner);
      }
      return [];
    },

    getOwnPropertyDescriptor(
      _: unknown,
      prop: string | symbol
    ): PropertyDescriptor | undefined {
      const inner = entrypoints.getCtxCache();
      if (inner) {
        return Reflect.getOwnPropertyDescriptor(inner, prop);
      }
      return undefined;
    },
  }
);

export const tracing = innerTracing;

export function abortIsolate(reason?: string): never {
  if (entrypoints.isExperimental) {
    entrypoints.abortIsolate(reason);
  } else {
    throw new Error(
      'abortIsolate() requires the "experimental" compatibility flag.'
    );
  }
}
