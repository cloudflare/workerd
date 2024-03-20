// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  DurableObject,
  RpcStub,
  RpcTarget,
  WorkerEntrypoint,
} from "cloudflare:workers";
import { expectTypeOf } from "expect-type";

class BoringClass {}

class TestCounter extends RpcTarget {
  constructor(private val = 0) {
    super();
  }
  get value() {
    return this.val;
  }
  increment(by = 1) {
    return (this.val += by);
  }
  copy() {
    return new TestCounter(this.val);
  }

  [Symbol.dispose]() {
    console.log("Disposing");
  }

  // Check can't use custom `dup()` method
  dup(x: number) {
    return x;
  }
}

const symbolMethod = Symbol("symbolMethod");

class TestEntrypoint extends WorkerEntrypoint<Env> {
  constructor(ctx: ExecutionContext, env: Env) {
    super(ctx, env);
  }

  fetch(request: Request) {
    return new Response(request.url);
  }
  async tail(_events: TraceItem[]) {}
  trace(_events: TraceItem[]) {}
  async scheduled(_controller: ScheduledController) {}
  queue(_batch: MessageBatch<number>) {}
  async test(_controller: TestController) {}

  private privateInstanceProperty = 0;
  private get privateProperty() {
    expectTypeOf(this.ctx).toEqualTypeOf<ExecutionContext>();
    expectTypeOf(this.env).toEqualTypeOf<Env>();

    return 1;
  }

  instanceProperty = "2";
  get property(): number {
    return 3;
  }
  get asyncProperty() {
    return Promise.resolve(true as const);
  }

  private privateMethod() {
    return 4;
  }
  [symbolMethod]() {
    return 5;
  }
  method() {
    return null;
  }
  async asyncMethod() {
    return true;
  }
  voidMethod(_x?: number) {}
  async asyncVoidMethod() {}

  functionMethod() {
    return (x: number) => x;
  }
  async asyncFunctionMethod() {
    return (x: number) => x;
  }
  async asyncAsyncFunctionMethod() {
    return async (x: number) => x;
  }

  functionWithExtrasMethod() {
    const fn = (x: number) => x;
    fn.y = "z";
    return fn;
  }

  async stubMethod(
    callback: RpcStub<(x: number) => number>,
    counter: RpcStub<TestCounter>,
    _map: Map<RpcStub<TestCounter>, RpcStub<TestCounter>>,
    _set: Set<RpcStub<TestCounter>>,
    _array: Array<RpcStub<TestCounter>>,
    _readonlyArray: ReadonlyArray<RpcStub<TestCounter>>,
    _object: { a: { b: RpcStub<TestCounter> } }
  ) {
    expectTypeOf(callback(1)).toEqualTypeOf<Promise<number>>(); // (always async)
    expectTypeOf(counter.value).toEqualTypeOf<Promise<number>>(); // (always async)
    const result = await callback(2);
    return result * 3;
  }

  get objectProperty() {
    return {
      w: 1,
      get x() {
        return 2;
      },
      y() {
        return 3;
      },
      async z(_a: boolean) {
        return 4;
      },
    };
  }

  get everySerializable() {
    return {
      undefined: undefined,
      null: null,
      boolean: false,
      number: 42,
      bigint: 1_000_000n,
      string: "hello",
      Int8Array: new Int8Array(),
      Uint8Array: new Uint8Array(),
      Uint8ClampedArray: new Uint8ClampedArray(),
      Int16Array: new Int16Array(),
      Uint16Array: new Uint16Array(),
      Int32Array: new Int32Array(),
      Uint32Array: new Uint32Array(),
      Float32Array: new Float32Array(),
      Float64Array: new Float64Array(),
      BigInt64Array: new BigInt64Array(),
      BigUint64Array: new BigUint64Array(),
      ArrayBuffer: new ArrayBuffer(4),
      DataView: new DataView(new ArrayBuffer(4)),
      Date: new Date(),
      Error: new Error(),
      EvalError: new EvalError(),
      RangeError: new RangeError(),
      ReferenceError: new ReferenceError(),
      SyntaxError: new SyntaxError(),
      TypeError: new TypeError(),
      URIError: new URIError(),
      RegExp: /abc/,
      Map: new Map([["a", 1]]),
      Set: new Set(["a"]),
      Array: [1, 2, 3],
      ReadonlyArray: [4, 5, 6] as const,
      Object: { a: { b: 1 } },
      ReadableStream: new ReadableStream<Uint8Array>(),
      WritableStream: new WritableStream<Uint8Array>(),
      Request: new Request("https://example.com"),
      Response: new Response(),
      Headers: new Headers(),
      Stub: new RpcStub(() => {}),
    };
  }
  get everyCompositeSerializable() {
    return {
      Map: new Map([[new TestCounter(), new TestCounter()]]),
      Set: new Set([new TestCounter()]),
      Array: [new TestCounter()],
      ReadonlyArray: [new TestCounter()] as const,
      Object: { a: { b: new TestCounter() } },
    };
  }
  get nonSerializable1() {
    return new BoringClass();
  }
  nonSerializable2() {
    return { a: new BoringClass() };
  }
  async nonSerializable3() {
    return new ReadableStream<string>();
  }

  [Symbol.dispose]() {
    console.log("Disposing");
  }
}

class TestObject extends DurableObject {
  async fetch(request: Request) {
    return new Response(request.url);
  }
  async alarm() {}
  webSocketMessage(_ws: WebSocket, _message: string | ArrayBuffer) {}
  async webSocketClose(
    _ws: WebSocket,
    _code: number,
    _reason: string,
    _wasClean: boolean
  ) {}
  webSocketError(_ws: WebSocket, _error: unknown) {}

  targetMethod() {
    return new TestCounter();
  }
  async asyncTargetMethod() {
    return new TestCounter();
  }

  [Symbol.dispose]() {
    console.log("Disposing");
  }
}

class TestNaughtyEntrypoint extends WorkerEntrypoint {
  // Check incorrectly typed methods
  // @ts-expect-error
  fetch(_request: Request) {
    return "body";
  }
  // @ts-expect-error
  async tail(_animal: "🐶") {}
  // @ts-expect-error
  trace(_draw: boolean) {}
  // @ts-expect-error
  async scheduled(_at: Date) {}
  // @ts-expect-error
  queue(_message: string) {}
  // @ts-expect-error
  async test(_x: number, _y: number) {}
}

class TestNaughtyObject extends DurableObject {
  // Check incorrectly typed methods
  // @ts-expect-error
  async fetch(url: string) {
    return new Response(url);
  }
  // @ts-expect-error
  async alarm(_x: boolean) {}
  // @ts-expect-error
  webSocketMessage(_x: boolean) {}
  // @ts-expect-error
  async webSocketClose(_x: boolean) {}
  // @ts-expect-error
  webSocketError(_x: boolean) {}
}

interface Env {
  REGULAR_SERVICE: Fetcher;
  RPC_SERVICE: Fetcher<TestEntrypoint>;
  NAUGHTY_SERVICE: Fetcher<TestNaughtyEntrypoint>;
  // @ts-expect-error `BoringClass` isn't an RPC capable type
  __INVALID_RPC_SERVICE: Fetcher<BoringClass>;

  REGULAR_OBJECT: DurableObjectNamespace;
  RPC_OBJECT: DurableObjectNamespace<TestObject>;
  NAUGHTY_OBJECT: DurableObjectNamespace<TestNaughtyObject>;
  // @ts-expect-error `BoringClass` isn't an RPC capable type
  __INVALID_OBJECT_1: DurableObjectNamespace<BoringClass>;
  // @ts-expect-error `TestEntrypoint` is a `WorkerEntrypoint`, not a `DurableObject`
  __INVALID_OBJECT_2: DurableObjectNamespace<TestEntrypoint>;
}

export default <ExportedHandler<Env>>{
  async fetch(_request, env, _ctx) {
    // Check non-RPC services and namespaces work as usual
    {
      const response = await env.REGULAR_SERVICE.fetch("https://example.com", {
        method: "POST",
      });
      expectTypeOf(response).toEqualTypeOf<Response>();

      const uniqueId = env.REGULAR_OBJECT.newUniqueId();
      expectTypeOf(uniqueId).toEqualTypeOf<DurableObjectId>();
      const nameId = env.REGULAR_OBJECT.newUniqueId();
      expectTypeOf(nameId).toEqualTypeOf<DurableObjectId>();
      const stringId = env.REGULAR_OBJECT.newUniqueId();
      expectTypeOf(stringId).toEqualTypeOf<DurableObjectId>();

      const stub = env.REGULAR_OBJECT.get(uniqueId);
      const objectResponse = await stub.fetch("https://example.com", {
        method: "POST",
      });
      expectTypeOf(objectResponse).toEqualTypeOf<Response>();
      expectTypeOf(stub.id).toEqualTypeOf<DurableObjectId>();
      expectTypeOf(stub.name).toEqualTypeOf<string | undefined>();
    }

    // Check RPC services and namespaces support standard methods (without overloads,
    // `toEqualTypeOf(...)` will fail if the function signature doesn't match *exactly*)
    {
      expectTypeOf(env.RPC_SERVICE.fetch).toEqualTypeOf<
        (input: RequestInfo, init?: RequestInit) => Promise<Response>
      >();
      expectTypeOf(env.RPC_SERVICE.connect).toEqualTypeOf<
        (address: SocketAddress | string, options?: SocketOptions) => Socket
      >();
      expectTypeOf(env.RPC_SERVICE.queue).toEqualTypeOf<
        (
          queueName: string,
          messages: ServiceBindingQueueMessage[]
        ) => Promise<FetcherQueueResult>
      >();
      expectTypeOf(env.RPC_SERVICE.scheduled).toEqualTypeOf<
        (options?: FetcherScheduledOptions) => Promise<FetcherScheduledResult>
      >();

      const stub = env.RPC_OBJECT.get(env.RPC_OBJECT.newUniqueId());
      expectTypeOf(stub.fetch).toEqualTypeOf<
        (input: RequestInfo, init?: RequestInit) => Promise<Response>
      >();
      expectTypeOf(stub.connect).toEqualTypeOf<
        (address: SocketAddress | string, options?: SocketOptions) => Socket
      >();
    }

    // Check cannot access `env` and `ctx` over RPC
    {
      // @ts-expect-error protected properties are not accessible
      env.RPC_SERVICE.env;
      // @ts-expect-error protected properties are not accessible
      env.RPC_SERVICE.ctx;

      const stub = env.RPC_OBJECT.get(env.RPC_OBJECT.newUniqueId());
      // @ts-expect-error protected properties are not accessible
      stub.env;
      // @ts-expect-error protected properties are not accessible
      stub.ctx;
    }

    // Check accessing properties (including using)
    {
      const s = env.RPC_SERVICE;

      // @ts-expect-error private properties are not accessible
      s.privateInstanceProperty;
      // @ts-expect-error private properties are not accessible
      s.privateProperty;

      // Unfortunately, TypeScript's structural typing makes it tricky to
      // differentiate between instance and prototype properties. This line
      // would fail at runtime. We should encourage people to use `private`
      // access modifiers when using TypeScript.
      // TODO(someday): try make this fail type checking
      expectTypeOf(s.instanceProperty).toEqualTypeOf<Promise<string>>();

      expectTypeOf(s.property).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(s.asyncProperty).toEqualTypeOf<Promise<true>>();
    }

    // Check calling methods (including using)
    {
      const s = env.RPC_SERVICE;

      // @ts-expect-error private methods are not accessible
      s.privateMethod;
      // @ts-expect-error symbol methods are not accessible
      s[symbolMethod];

      expectTypeOf(s.method()).toEqualTypeOf<Promise<null>>(); // (always async)
      expectTypeOf(await s.asyncMethod()).toEqualTypeOf<boolean>();

      expectTypeOf(s.voidMethod).toEqualTypeOf<(x?: number) => Promise<void>>(); // (always async)
      expectTypeOf(s.asyncVoidMethod).toEqualTypeOf<() => Promise<void>>();
    }

    // Check methods returning functions (including pipelining/ERM)
    {
      const s = env.RPC_SERVICE;

      expectTypeOf(s.functionMethod()).toMatchTypeOf<Promise<unknown>>();
      using f1 = await s.functionMethod();
      expectTypeOf(f1).toEqualTypeOf<RpcStub<(x: number) => number>>();
      expectTypeOf(f1.dup()).toEqualTypeOf(f1);
      expectTypeOf(f1(1)).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(s.functionMethod()(1)).toEqualTypeOf<Promise<number>>(); // (pipelining)

      expectTypeOf(s.asyncFunctionMethod()).toMatchTypeOf<Promise<unknown>>();
      using f2 = await s.asyncFunctionMethod();
      expectTypeOf(f2).toEqualTypeOf<RpcStub<(x: number) => number>>();
      expectTypeOf(s.asyncFunctionMethod()(1)).toEqualTypeOf<Promise<number>>(); // (pipelining)

      expectTypeOf(s.asyncAsyncFunctionMethod()).toMatchTypeOf<
        Promise<unknown>
      >();
      using f3 = await s.asyncAsyncFunctionMethod();
      expectTypeOf(f3).toEqualTypeOf<RpcStub<(x: number) => Promise<number>>>();
      expectTypeOf(f3(1)).toEqualTypeOf<Promise<number>>();
      expectTypeOf(s.asyncAsyncFunctionMethod()(1)).toEqualTypeOf<
        Promise<number>
      >(); // (pipelining)

      expectTypeOf(s.functionWithExtrasMethod()).toMatchTypeOf<
        Promise<unknown>
      >();
      using f4 = await s.functionWithExtrasMethod();
      type F4 = { (x: number): number; y: string };
      expectTypeOf(f4).toEqualTypeOf<RpcStub<F4>>();
      expectTypeOf(f4(1)).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(f4.y).toEqualTypeOf<Promise<string>>(); // (always async)
      expectTypeOf(s.functionWithExtrasMethod()(1)).toEqualTypeOf<
        Promise<number>
      >(); // (pipelining)
      expectTypeOf(s.functionWithExtrasMethod().y).toEqualTypeOf<
        Promise<string>
      >(); // (pipelining)
    }

    // Check methods returning objects (including pipelining/ERM)
    {
      const s = env.RPC_SERVICE;

      expectTypeOf(s.objectProperty).toMatchTypeOf<Promise<unknown>>();
      using o = await s.objectProperty;
      expectTypeOf(o.w).toEqualTypeOf<number>();
      expectTypeOf(o.x).toEqualTypeOf<number>();
      expectTypeOf(o.y).toEqualTypeOf<RpcStub<() => number>>();
      expectTypeOf(o.y()).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(o.z).toEqualTypeOf<
        RpcStub<(a: boolean) => Promise<number>>
      >();
      expectTypeOf(o.z(true)).toEqualTypeOf<Promise<number>>();

      expectTypeOf(s.objectProperty.w).toEqualTypeOf<Promise<number>>(); // (pipelining)
      expectTypeOf(s.objectProperty.x).toEqualTypeOf<Promise<number>>(); // (pipelining)
      expectTypeOf(s.objectProperty.y()).toEqualTypeOf<Promise<number>>(); // (pipelining)
      expectTypeOf(s.objectProperty.z(false)).toEqualTypeOf<Promise<number>>(); // (pipelining)

      expectTypeOf(s.everySerializable).not.toBeNever();
      expectTypeOf(s.nonSerializable1).toBeNever();
      expectTypeOf(s.nonSerializable2).returns.toBeNever();
      expectTypeOf(s.nonSerializable3).returns.toBeNever();

      // Verify serializable objects without any stubs are still disposable
      (await s.everySerializable)[Symbol.dispose]();

      // Verify types passed by value can still be pipelined
      expectTypeOf(s.everySerializable.Object.a.b).toEqualTypeOf<
        Promise<number>
      >;
      // TODO(now): these next two don't actually work, should they?
      expectTypeOf(s.everySerializable.Array[0]).toEqualTypeOf<Promise<number>>;
      expectTypeOf(await s.everySerializable.Map.get("a")).toEqualTypeOf<
        number | undefined
      >;

      // Verify stubable types replaced with stubs
      using ecs = await s.everyCompositeSerializable;
      expectTypeOf(ecs.Map).toEqualTypeOf<
        Map<RpcStub<TestCounter>, RpcStub<TestCounter>>
      >();
      expectTypeOf(ecs.Set).toEqualTypeOf<Set<RpcStub<TestCounter>>>();
      expectTypeOf(ecs.Array).toEqualTypeOf<Array<RpcStub<TestCounter>>>();
      expectTypeOf(ecs.ReadonlyArray).toEqualTypeOf<
        ReadonlyArray<RpcStub<TestCounter>>
      >();
      expectTypeOf(ecs.Object).toEqualTypeOf<{
        a: { b: RpcStub<TestCounter> };
      }>();
    }

    // Check methods returning targets (including pipelining/ERM)
    {
      const s = env.RPC_OBJECT.get(env.RPC_OBJECT.newUniqueId());

      expectTypeOf(s.targetMethod()).toMatchTypeOf<Promise<unknown>>();
      using t1 = await s.targetMethod();
      expectTypeOf(t1).toEqualTypeOf<RpcStub<TestCounter>>();
      expectTypeOf(t1.dup()).toEqualTypeOf(t1);
      expectTypeOf(t1.value).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(t1.increment()).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(t1.increment(1)).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(t1.copy()).toMatchTypeOf<Promise<RpcStub<TestCounter>>>();

      expectTypeOf(s.targetMethod().copy().increment(1)).toEqualTypeOf<
        Promise<number>
      >(); // (pipelining)

      expectTypeOf(s.asyncTargetMethod()).toMatchTypeOf<Promise<unknown>>();
      using t2 = await s.asyncTargetMethod();
      expectTypeOf(t2).toEqualTypeOf<RpcStub<TestCounter>>();
    }

    // Check methods accepting stubs
    {
      expectTypeOf(env.RPC_SERVICE.stubMethod).toEqualTypeOf<
        (
          callback: (x: number) => number,
          counter: TestCounter,
          map: Map<TestCounter, TestCounter>,
          set: Set<TestCounter>,
          array: Array<TestCounter>,
          readonlyArray: ReadonlyArray<TestCounter>,
          object: { a: { b: TestCounter } }
        ) => Promise<number>
      >();
    }

    // Check loopback stubs support same operations as remote stubs
    {
      const stub = new RpcStub(new TestCounter(42));
      expectTypeOf(stub).toEqualTypeOf<RpcStub<TestCounter>>();
      expectTypeOf(stub.value).toEqualTypeOf<Promise<number>>(); // (always async)
      expectTypeOf(stub.copy().increment(1)).toEqualTypeOf<Promise<number>>(); // (pipelining)

      // @ts-expect-error requires stubable type
      new RpcStub(1);
    }

    // Check can't override `dup()`
    {
      const stub = new RpcStub(new TestCounter(42));
      expectTypeOf(stub.dup).toEqualTypeOf<() => RpcStub<TestCounter>>();
    }

    return new Response();
  },
};
