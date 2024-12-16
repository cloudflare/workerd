// Namespace for RPC utility types. Unfortunately, we can't use a `module` here as these types need
// to referenced by `Fetcher`. This is included in the "importable" version of the types which
// strips all `module` blocks.
declare namespace Rpc {
  // Branded types for identifying `WorkerEntrypoint`/`DurableObject`/`Target`s.
  // TypeScript uses *structural* typing meaning anything with the same shape as type `T` is a `T`.
  // For the classes exported by `cloudflare:workers` we want *nominal* typing (i.e. we only want to
  // accept `WorkerEntrypoint` from `cloudflare:workers`, not any other class with the same shape)
  export const __RPC_STUB_BRAND: "__RPC_STUB_BRAND";
  export const __RPC_TARGET_BRAND: "__RPC_TARGET_BRAND";
  export const __WORKER_ENTRYPOINT_BRAND: "__WORKER_ENTRYPOINT_BRAND";
  export const __DURABLE_OBJECT_BRAND: "__DURABLE_OBJECT_BRAND";
  export const __WORKFLOW_ENTRYPOINT_BRAND: "__WORKFLOW_ENTRYPOINT_BRAND";
  export interface RpcTargetBranded {
    [__RPC_TARGET_BRAND]: never;
  }
  export interface WorkerEntrypointBranded {
    [__WORKER_ENTRYPOINT_BRAND]: never;
  }
  export interface DurableObjectBranded {
    [__DURABLE_OBJECT_BRAND]: never;
  }
  export interface WorkflowEntrypointBranded {
    [__WORKFLOW_ENTRYPOINT_BRAND]: never;
  }
  export type EntrypointBranded =
    | WorkerEntrypointBranded
    | DurableObjectBranded
    | WorkflowEntrypointBranded;

  // Types that can be used through `Stub`s
  export type Stubable = RpcTargetBranded | ((...args: any[]) => any);

  // Types that can be passed over RPC
  // The reason for using a generic type here is to build a serializable subset of structured
  //   cloneable composite types. This allows types defined with the "interface" keyword to pass the
  //   serializable check as well. Otherwise, only types defined with the "type" keyword would pass.
  type Serializable<T> =
    // Structured cloneables
    | void
    | undefined
    | null
    | boolean
    | number
    | bigint
    | string
    | TypedArray
    | ArrayBuffer
    | DataView
    | Date
    | Error
    | RegExp
    // Structured cloneable composites
    | Map<
        T extends Map<infer U, unknown> ? Serializable<U> : never,
        T extends Map<unknown, infer U> ? Serializable<U> : never
      >
    | Set<T extends Set<infer U> ? Serializable<U> : never>
    | ReadonlyArray<T extends ReadonlyArray<infer U> ? Serializable<U> : never>
    | {
        [K in keyof T]: K extends number | string ? Serializable<T[K]> : never;
      }
    // Special types
    | ReadableStream<Uint8Array>
    | WritableStream<Uint8Array>
    | Request
    | Response
    | Headers
    | Stub<Stubable>
    // Serialized as stubs, see `Stubify`
    | Stubable;

  // Base type for all RPC stubs, including common memory management methods.
  // `T` is used as a marker type for unwrapping `Stub`s later.
  interface StubBase<T extends Stubable> extends Disposable {
    [__RPC_STUB_BRAND]: T;
    dup(): this;
  }
  export type Stub<T extends Stubable> = Provider<T> & StubBase<T>;

  // Recursively rewrite all `Stubable` types with `Stub`s
  // prettier-ignore
  type Stubify<T> =
    T extends Stubable ? Stub<T>
    : T extends Map<infer K, infer V> ? Map<Stubify<K>, Stubify<V>>
    : T extends Set<infer V> ? Set<Stubify<V>>
    : T extends Array<infer V> ? Array<Stubify<V>>
    : T extends ReadonlyArray<infer V> ? ReadonlyArray<Stubify<V>>
    // When using "unknown" instead of "any", interfaces are not stubified.
    : T extends { [key: string | number]: any } ? { [K in keyof T]: Stubify<T[K]> }
    : T;

  // Recursively rewrite all `Stub<T>`s with the corresponding `T`s.
  // Note we use `StubBase` instead of `Stub` here to avoid circular dependencies:
  // `Stub` depends on `Provider`, which depends on `Unstubify`, which would depend on `Stub`.
  // prettier-ignore
  type Unstubify<T> =
    T extends StubBase<infer V> ? V
    : T extends Map<infer K, infer V> ? Map<Unstubify<K>, Unstubify<V>>
    : T extends Set<infer V> ? Set<Unstubify<V>>
    : T extends Array<infer V> ? Array<Unstubify<V>>
    : T extends ReadonlyArray<infer V> ? ReadonlyArray<Unstubify<V>>
    : T extends { [key: string | number]: unknown } ? { [K in keyof T]: Unstubify<T[K]> }
    : T;
  type UnstubifyAll<A extends any[]> = { [I in keyof A]: Unstubify<A[I]> };

  // Utility type for adding `Provider`/`Disposable`s to `object` types only.
  // Note `unknown & T` is equivalent to `T`.
  type MaybeProvider<T> = T extends object ? Provider<T> : unknown;
  type MaybeDisposable<T> = T extends object ? Disposable : unknown;

  // Type for method return or property on an RPC interface.
  // - Stubable types are replaced by stubs.
  // - Serializable types are passed by value, with stubable types replaced by stubs
  //   and a top-level `Disposer`.
  // Everything else can't be passed over PRC.
  // Technically, we use custom thenables here, but they quack like `Promise`s.
  // Intersecting with `(Maybe)Provider` allows pipelining.
  // prettier-ignore
  type Result<R> =
    R extends Stubable ? Promise<Stub<R>> & Provider<R>
    : R extends Serializable<R> ? Promise<Stubify<R> & MaybeDisposable<R>> & MaybeProvider<R>
    : never;

  // Type for method or property on an RPC interface.
  // For methods, unwrap `Stub`s in parameters, and rewrite returns to be `Result`s.
  // Unwrapping `Stub`s allows calling with `Stubable` arguments.
  // For properties, rewrite types to be `Result`s.
  // In each case, unwrap `Promise`s.
  type MethodOrProperty<V> = V extends (...args: infer P) => infer R
    ? (...args: UnstubifyAll<P>) => Result<Awaited<R>>
    : Result<Awaited<V>>;

  // Type for the callable part of an `Provider` if `T` is callable.
  // This is intersected with methods/properties.
  type MaybeCallableProvider<T> = T extends (...args: any[]) => any
    ? MethodOrProperty<T>
    : unknown;

  // Base type for all other types providing RPC-like interfaces.
  // Rewrites all methods/properties to be `MethodOrProperty`s, while preserving callable types.
  // `Reserved` names (e.g. stub method names like `dup()`) and symbols can't be accessed over RPC.
  export type Provider<
    T extends object,
    Reserved extends string = never,
  > = MaybeCallableProvider<T> & {
    [K in Exclude<
      keyof T,
      Reserved | symbol | keyof StubBase<never>
    >]: MethodOrProperty<T[K]>;
  };
}

declare module "cloudflare:workers" {
  export type RpcStub<T extends Rpc.Stubable> = Rpc.Stub<T>;
  export const RpcStub: {
    new <T extends Rpc.Stubable>(value: T): Rpc.Stub<T>;
  };

  export abstract class RpcTarget implements Rpc.RpcTargetBranded {
    [Rpc.__RPC_TARGET_BRAND]: never;
  }

  // `protected` fields don't appear in `keyof`s, so can't be accessed over RPC

  export abstract class WorkerEntrypoint<Env = unknown>
    implements Rpc.WorkerEntrypointBranded
  {
    [Rpc.__WORKER_ENTRYPOINT_BRAND]: never;

    protected ctx: ExecutionContext;
    protected env: Env;
    constructor(ctx: ExecutionContext, env: Env);

    fetch?(request: Request): Response | Promise<Response>;
    tail?(events: TraceItem[]): void | Promise<void>;
    trace?(traces: TraceItem[]): void | Promise<void>;
    scheduled?(controller: ScheduledController): void | Promise<void>;
    queue?(batch: MessageBatch<unknown>): void | Promise<void>;
    test?(controller: TestController): void | Promise<void>;
  }

  export abstract class DurableObject<Env = unknown>
    implements Rpc.DurableObjectBranded
  {
    [Rpc.__DURABLE_OBJECT_BRAND]: never;

    protected ctx: DurableObjectState;
    protected env: Env;
    constructor(ctx: DurableObjectState, env: Env);

    fetch?(request: Request): Response | Promise<Response>;
    alarm?(alarmInfo?: AlarmInvocationInfo): void | Promise<void>;
    webSocketMessage?(
      ws: WebSocket,
      message: string | ArrayBuffer
    ): void | Promise<void>;
    webSocketClose?(
      ws: WebSocket,
      code: number,
      reason: string,
      wasClean: boolean
    ): void | Promise<void>;
    webSocketError?(ws: WebSocket, error: unknown): void | Promise<void>;
  }

  export type WorkflowDurationLabel =
    | "second"
    | "minute"
    | "hour"
    | "day"
    | "week"
    | "month"
    | "year";

  export type WorkflowSleepDuration =
    | `${number} ${WorkflowDurationLabel}${"s" | ""}`
    | number;

  export type WorkflowDelayDuration = WorkflowSleepDuration;

  export type WorkflowTimeoutDuration = WorkflowSleepDuration;

  export type WorkflowBackoff = "constant" | "linear" | "exponential";

  export type WorkflowStepConfig = {
    retries?: {
      limit: number;
      delay: WorkflowDelayDuration | number;
      backoff?: WorkflowBackoff;
    };
    timeout?: WorkflowTimeoutDuration | number;
  };

  export type WorkflowEvent<T> = {
    payload: Readonly<T>;
    timestamp: Date;
    instanceId: string;
  };

  export abstract class WorkflowStep {
    do<T extends Rpc.Serializable<T>>(name: string, callback: () => Promise<T>): Promise<T>;
    do<T extends Rpc.Serializable<T>>(name: string, config: WorkflowStepConfig, callback: () => Promise<T>): Promise<T>;
    sleep: (name: string, duration: WorkflowSleepDuration) => Promise<void>;
    sleepUntil: (name: string, timestamp: Date | number) => Promise<void>;
  }

  export abstract class WorkflowEntrypoint<
    Env = unknown,
    T extends Rpc.Serializable<T> | unknown = unknown,
  > implements Rpc.WorkflowEntrypointBranded
  {
    [Rpc.__WORKFLOW_ENTRYPOINT_BRAND]: never;

    protected ctx: ExecutionContext;
    protected env: Env;

    run(event: Readonly<WorkflowEvent<T>>, step: WorkflowStep): Promise<unknown>;
  }
}
