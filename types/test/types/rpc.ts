// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  cache,
  DurableObject,
  RpcStub,
  RpcTarget,
  WorkerEntrypoint,
  type WorkflowBackoff,
  type WorkflowCronSchedule,
  type WorkflowDelayDuration,
  type WorkflowDelayFunction,
  type WorkflowDynamicDelayContext,
  type WorkflowEvent,
  type WorkflowStep,
  type WorkflowStepConfig,
  type WorkflowStepContext,
  type WorkflowStepSensitivity,
} from 'cloudflare:workers';
import { expectTypeOf } from 'expect-type';

// Check `cache` export from `cloudflare:workers` has the expected type.
expectTypeOf(cache).toEqualTypeOf<CacheContext>();
expectTypeOf(cache.purge).toEqualTypeOf<
  (options: CachePurgeOptions) => Promise<CachePurgeResult>
>();

type TestType = {
  fieldString: string;
  fieldCallback: (p: string) => number;
  fieldBasicMap: Map<string, number>;
  fieldComplexMap: Map<
    string,
    {
      fieldString: string;
      fieldCallback: (p: string) => number;
    }
  >;
  fieldSet: Set<string>;
  fieldSubLevel: {
    fieldString: string;
    fieldCallback: (p: string) => number;
  };
};

interface ABasicInterface {
  fieldString: string;
  fieldCallback: (p: string) => number;
}

interface TestInterface extends ABasicInterface {
  fieldBasicMap: Map<string, number>;
  fieldComplexMap: Map<string, ABasicInterface>;
  fieldSet: Set<string>;
  fieldSubLevelInline: {
    fieldString: string;
    fieldCallback: (p: string) => number;
  };
  fieldSubLevelInterface: ABasicInterface;
}

interface NonSerializableInterface {
  field: ReadableStream<string>;
}

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
    console.log('Disposing');
  }

  // Check can't use custom `dup()` method
  dup(x: number) {
    return x;
  }
}

const symbolMethod = Symbol('symbolMethod');

type Props = {myProp: number};

class TestEntrypoint extends WorkerEntrypoint<Env, Props> {
  constructor(ctx: ExecutionContext<Props>, env: Env) {
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
    expectTypeOf(this.ctx).toEqualTypeOf<ExecutionContext<Props>>();
    expectTypeOf(this.env).toEqualTypeOf<Env>();

    return 1;
  }

  instanceProperty = '2';
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
    fn.y = 'z';
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
      string: 'hello',
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
      Map: new Map([['a', 1]]),
      Set: new Set(['a']),
      Array: [1, 2, 3],
      ReadonlyArray: [4, 5, 6] as const,
      Object: { a: { b: 1 } },
      ReadableStream: new ReadableStream<Uint8Array>(),
      WritableStream: new WritableStream<Uint8Array>(),
      Request: new Request('https://example.com'),
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

  methodReturnsTypeObject(): TestType {
    return {
      fieldString: 'a',
      fieldCallback: (p: string) => 1,
      fieldBasicMap: new Map([['b', 2]]),
      fieldComplexMap: new Map([
        [
          'c',
          {
            fieldString: 'd',
            fieldCallback: (p: string) => 3,
          },
        ],
      ]),
      fieldSet: new Set(['e']),
      fieldSubLevel: {
        fieldString: 'f',
        fieldCallback: (p: string) => 4,
      },
    };
  }
  methodReturnsInterfaceObject(): TestInterface {
    return {
      fieldString: 'a',
      fieldCallback: (p: string) => 1,
      fieldBasicMap: new Map([['b', 2]]),
      fieldComplexMap: new Map([
        [
          'c',
          {
            fieldString: 'd',
            fieldCallback: (p: string) => 3,
          },
        ],
      ]),
      fieldSet: new Set(['e']),
      fieldSubLevelInline: {
        fieldString: 'f',
        fieldCallback: (p: string) => 4,
      },
      fieldSubLevelInterface: {
        fieldString: 'e',
        fieldCallback: (p: string) => 5,
      },
    };
  }

  nonSerializable1() {
    return new ReadableStream<string>();
  }
  nonSerializable2() {
    return { field: new ReadableStream<string>() };
  }
  nonSerializable3(): NonSerializableInterface {
    return { field: new ReadableStream<string>() };
  }

  [Symbol.dispose]() {
    console.log('Disposing');
  }
}

class TestObject extends DurableObject {
  async fetch(request: Request) {
    return new Response(request.url);
  }
  async alarm() {}

  complexTypes() {
    return {
      undefined: undefined,
      void: void 0,
      null: null,
      boolean: true,
      number: 1,
      bigint: BigInt(4),
      string: 'string',
      ArrayBuffer: new ArrayBuffer(0),
      DataView: new DataView(new ArrayBuffer(0)),
      Date: new Date(),
      Error: new Error(),
      RegExp: new RegExp(''),
      ReadableStream: new ReadableStream(),
      WritableStream: new WritableStream(),
      Request: new Request('https://example.com'),
      Response: new Response(),
      Headers: new Headers(),
      nested: {
        undefined: undefined,
        void: void 0,
        null: null,
        boolean: true,
        number: 1,
        bigint: BigInt(4),
        string: 'string',
        ArrayBuffer: new ArrayBuffer(0),
        DataView: new DataView(new ArrayBuffer(0)),
        Date: new Date(),
        Error: new Error(),
        RegExp: new RegExp(''),
        ReadableStream: new ReadableStream(),
        WritableStream: new WritableStream(),
        Request: new Request('https://example.com'),
        Response: new Response(),
        Headers: new Headers(),
      },
    };
  }

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
    console.log('Disposing');
  }
}

class TestAlarmObject extends DurableObject {
  // Can declare alarm method consuming optional alarmInfo parameter
  async alarm(alarmInfo?: AlarmInvocationInfo) {
    if (alarmInfo !== undefined) {
      const _isRetry: boolean = alarmInfo.isRetry;
      const _retryCount: number = alarmInfo.retryCount;
      const _scheduledTime: number = alarmInfo.scheduledTime;
    }
  }

  // User code can invoke alarm() directly, if desired.
  async runAlarmVoid(): Promise<void> {
    return await this.alarm();
  }
  async runAlarmInfo(): Promise<void> {
    return await this.alarm({
      isRetry: true,
      retryCount: 1,
      scheduledTime: 1000,
    });
  }
}

class TestNaughtyEntrypoint extends WorkerEntrypoint {
  // Check incorrectly typed methods
  // @ts-expect-error
  fetch(_request: Request) {
    return 'body';
  }
  // @ts-expect-error
  async tail(_animal: '🐶') {}
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
  REGULAR_SERVICE: Service;
  RPC_SERVICE: Service<TestEntrypoint>;
  TYPEOF_RPC_SERVICE: Service<typeof TestEntrypoint>;
  NAUGHTY_SERVICE: Service<TestNaughtyEntrypoint>;
  // @ts-expect-error `BoringClass` isn't an RPC capable type
  __INVALID_RPC_SERVICE_1: Service<BoringClass>;

  REGULAR_OBJECT: DurableObjectNamespace;
  RPC_OBJECT: DurableObjectNamespace<TestObject>;
  ALARM_OBJECT: DurableObjectNamespace<TestAlarmObject>;
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
      const response = await env.REGULAR_SERVICE.fetch('https://example.com', {
        method: 'POST',
      });
      expectTypeOf(response).toEqualTypeOf<Response>();

      const uniqueId = env.REGULAR_OBJECT.newUniqueId();
      expectTypeOf(uniqueId).toEqualTypeOf<DurableObjectId>();
      const nameId = env.REGULAR_OBJECT.newUniqueId();
      expectTypeOf(nameId).toEqualTypeOf<DurableObjectId>();
      const stringId = env.REGULAR_OBJECT.newUniqueId();
      expectTypeOf(stringId).toEqualTypeOf<DurableObjectId>();

      const stub = env.REGULAR_OBJECT.get(uniqueId);
      const objectResponse = await stub.fetch('https://example.com', {
        method: 'POST',
      });
      expectTypeOf(objectResponse).toEqualTypeOf<Response>();
      expectTypeOf(stub.id).toEqualTypeOf<DurableObjectId>();
      expectTypeOf(stub.name).toEqualTypeOf<string | undefined>();
    }

    // Check RPC services and namespaces support standard methods (without overloads,
    // `toEqualTypeOf(...)` will fail if the function signature doesn't match *exactly*)
    {
      expectTypeOf(env.RPC_SERVICE.fetch).toEqualTypeOf<
        (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>
      >();
      expectTypeOf(env.RPC_SERVICE.connect).toEqualTypeOf<
        (address: SocketAddress | string, options?: SocketOptions) => Socket
      >();
      expectTypeOf(env.RPC_SERVICE.queue).toEqualTypeOf<
        (
          queueName: string,
          messages: ServiceBindingQueueMessage[],
          metadata?: MessageBatchMetadata
        ) => Promise<FetcherQueueResult>
      >();
      expectTypeOf(env.RPC_SERVICE.scheduled).toEqualTypeOf<
        (options?: FetcherScheduledOptions) => Promise<FetcherScheduledResult>
      >();

      const stub = env.RPC_OBJECT.get(env.RPC_OBJECT.newUniqueId());
      expectTypeOf(stub.fetch).toEqualTypeOf<
        (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>
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

      // Verify serializable composite objects defined with "type" keyword
      const oType = await s.methodReturnsTypeObject();
      expectTypeOf(oType).not.toBeNever();
      expectTypeOf(oType.fieldString).toEqualTypeOf<string>();
      expectTypeOf(oType.fieldCallback).toEqualTypeOf<
        RpcStub<(p: string) => number>
      >(); // stubified
      expectTypeOf(oType.fieldBasicMap).toEqualTypeOf<Map<string, number>>();
      expectTypeOf(oType.fieldComplexMap).toEqualTypeOf<
        Map<
          string,
          {
            fieldString: string;
            fieldCallback: RpcStub<(p: string) => number>; // stubified
          }
        >
      >();
      expectTypeOf(oType.fieldSet).toEqualTypeOf<Set<string>>();
      expectTypeOf(oType.fieldSubLevel.fieldString).toEqualTypeOf<string>();
      expectTypeOf(oType.fieldSubLevel.fieldCallback).toEqualTypeOf<
        RpcStub<(p: string) => number>
      >(); // stubified

      // Verify serializable composite objects defined with "interface" keyword
      const oInterface = await s.methodReturnsInterfaceObject();
      expectTypeOf(oInterface).not.toBeNever();
      expectTypeOf(oInterface.fieldString).toEqualTypeOf<string>();
      expectTypeOf(oInterface.fieldCallback).toEqualTypeOf<
        RpcStub<(p: string) => number>
      >(); // stubified
      expectTypeOf(oInterface.fieldBasicMap).toEqualTypeOf<
        Map<string, number>
      >();
      expectTypeOf(oInterface.fieldComplexMap).toEqualTypeOf<
        Map<
          string,
          {
            fieldString: string;
            fieldCallback: RpcStub<(p: string) => number>; // stubified
          }
        >
      >();
      expectTypeOf(oInterface.fieldSet).toEqualTypeOf<Set<string>>();
      expectTypeOf(
        oInterface.fieldSubLevelInline.fieldString
      ).toEqualTypeOf<string>();
      expectTypeOf(oInterface.fieldSubLevelInline.fieldCallback).toEqualTypeOf<
        RpcStub<(p: string) => number>
      >(); // stubified
      expectTypeOf(
        oInterface.fieldSubLevelInterface.fieldString
      ).toEqualTypeOf<string>();
      expectTypeOf(
        oInterface.fieldSubLevelInterface.fieldCallback
      ).toEqualTypeOf<RpcStub<(p: string) => number>>(); // stubified

      expectTypeOf(s.nonSerializable1).returns.toBeNever();
      // Note: Since one of the object's members is non-serializable,
      //   the entire object is resolved as 'never'.
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
      expectTypeOf(await s.everySerializable.Map.get('a')).toEqualTypeOf<
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

    // Check methods returning base types are not stubified
    {
      const s = env.RPC_OBJECT.get(env.RPC_OBJECT.newUniqueId());

      expectTypeOf(s.fetch(_request)).toMatchTypeOf<Promise<Response>>();
      expectTypeOf(s.complexTypes()).toMatchTypeOf<
        Promise<{
          undefined: undefined;
          void: void;
          null: null;
          boolean: boolean;
          number: number;
          bigint: bigint;
          string: string;
          ArrayBuffer: ArrayBuffer;
          DataView: DataView;
          Date: Date;
          Error: Error;
          RegExp: RegExp;
          ReadableStream: ReadableStream;
          WritableStream: WritableStream;
          Request: Request;
          Response: Response;
          Headers: Headers;
          nested: {
            undefined: undefined;
            void: void;
            null: null;
            boolean: boolean;
            number: number;
            bigint: bigint;
            string: string;
            ArrayBuffer: ArrayBuffer;
            DataView: DataView;
            Date: Date;
            Error: Error;
            RegExp: RegExp;
            ReadableStream: ReadableStream;
            WritableStream: WritableStream;
            Request: Request;
            Response: Response;
            Headers: Headers;
          };
        }>
      >;
    }

    return new Response();
  },
};

declare const workflowStep: WorkflowStep;

expectTypeOf(
  workflowStep.do('step with rollback', async (): Promise<string> => 'ok', {
    rollback: async (rollbackCtx) => {
      expectTypeOf(rollbackCtx.ctx).toEqualTypeOf<WorkflowStepContext>();
      expectTypeOf(rollbackCtx.error).toEqualTypeOf<Error>();
      expectTypeOf(rollbackCtx.output).toEqualTypeOf<string | undefined>();
      expectTypeOf(rollbackCtx.stepName).toEqualTypeOf<string>();
    },
  })
).toMatchTypeOf<Promise<string>>();

workflowStep.do(
  'configured rollback',
  {retries: {limit: 0, delay: 0}},
  async (): Promise<string> => 'ok',
  {
    rollback: async (rollbackCtx) => {
      expectTypeOf(rollbackCtx.ctx).toEqualTypeOf<WorkflowStepContext>();
      expectTypeOf(rollbackCtx.output).toEqualTypeOf<string | undefined>();
      expectTypeOf(rollbackCtx.stepName).toEqualTypeOf<string>();
    },
    rollbackConfig: {retries: {limit: 0, delay: 0}},
  }
);

workflowStep.do('rollback with timeout only', async (): Promise<string> => 'ok', {
  rollback: async () => {},
  rollbackConfig: {timeout: '10 seconds'},
});

// @ts-expect-error rollbackConfig only accepts retries and timeout
workflowStep.do('rollback config with sensitivity', async () => 'ok', {
  rollback: async () => {},
  rollbackConfig: {sensitive: 'output'},
});

// @ts-expect-error rollback options require a rollback handler
workflowStep.do('empty rollback options', async () => 'ok', {});

// @ts-expect-error rollbackConfig requires a rollback handler
workflowStep.do('rollback config without handler', async () => 'ok', {
  rollbackConfig: {retries: {limit: 0, delay: 0}},
});

expectTypeOf(
  workflowStep.do('no rollback 2-arg', async (): Promise<string> => 'ok')
).toMatchTypeOf<Promise<string>>();

expectTypeOf(
  workflowStep.do(
    'no rollback 3-arg',
    {retries: {limit: 1, delay: 0}},
    async (): Promise<string> => 'ok'
  )
).toMatchTypeOf<Promise<string>>();

// A static delay is surfaced (present) in the step context config.
workflowStep.do(
  'static delay context',
  {retries: {limit: 1, delay: '1 minute'}},
  async (ctx): Promise<string> => {
    expectTypeOf(ctx.config.retries).toMatchTypeOf<
      {delay: WorkflowDelayDuration | number} | undefined
    >();
    expectTypeOf(ctx.config.retries?.delay).toEqualTypeOf<
      WorkflowDelayDuration | number | undefined
    >();
    return 'ok';
  }
);

// A delay function is accepted, and the step context omits the (dynamic) delay.
// When `delay` is a function, it must NOT be present on the config exposed in
// either context surface: the delay function's `input.ctx` and the step callback's
// `ctx`. Both are asserted exactly (so a re-added `delay` fails the equality check)
// and guarded by `@ts-expect-error` (so a re-added `delay` makes the now-unused
// directive fail to compile).
workflowStep.do(
  'dynamic delay context',
  {
    retries: {
      limit: 1,
      delay: (input): WorkflowDelayDuration => {
        expectTypeOf(input).toEqualTypeOf<WorkflowDynamicDelayContext>();
        expectTypeOf(input.ctx).toEqualTypeOf<
          WorkflowStepContext<WorkflowDelayFunction>
        >();
        expectTypeOf(input.error).toEqualTypeOf<Error>();
        // `delay` is absent from the dynamic-delay context config (read-side, exact).
        expectTypeOf(input.ctx.config.retries).toEqualTypeOf<
          {limit: number; backoff?: WorkflowBackoff} | undefined
        >();
        return '1 minute';
      },
    },
  },
  async (ctx): Promise<string> => {
    // `delay` is absent here too because it was supplied as a function.
    expectTypeOf(ctx.config.retries).toEqualTypeOf<
      {limit: number; backoff?: WorkflowBackoff} | undefined
    >();
    return 'ok';
  }
);

// An async delay function is also accepted.
workflowStep.do(
  'async dynamic delay',
  {retries: {limit: 1, delay: async (): Promise<WorkflowDelayDuration> => 5}},
  async (): Promise<string> => 'ok'
);

// rollbackConfig also accepts a delay function.
workflowStep.do('rollback delay function', async (): Promise<string> => 'ok', {
  rollback: async () => {},
  rollbackConfig: {retries: {limit: 0, delay: () => 0}},
});

// No-config (2-arg) overload uses the default context: a static delay, so
// `config.retries.delay` is present.
workflowStep.do('no config delay present', async (ctx): Promise<string> => {
  expectTypeOf(ctx.config.retries?.delay).toEqualTypeOf<
    WorkflowDelayDuration | number | undefined
  >();
  return 'ok';
});

// A standalone delay function conforms to WorkflowDelayFunction and its `ctx`
// is the recursive dynamic-delay step context.
const dynamicDelay: WorkflowDelayFunction = ({ctx, error}) => {
  expectTypeOf(ctx).toEqualTypeOf<WorkflowStepContext<WorkflowDelayFunction>>();
  expectTypeOf(error).toEqualTypeOf<Error>();
  return '30 seconds';
};
void dynamicDelay;

// ---------------------------------------------------------------------------
// Regression coverage (WOR-1364) and `step.do` overload matrix.
//
// The config overloads discriminate on the shape of `config.retries.delay`
// (static duration vs. delay function) via distinct single-type-parameter
// overloads, plus a broad `WorkflowStepConfig` fallback. This fixes the
// regression where a single explicit type argument (the return type) combined
// with a config object failed to resolve to a config overload, and it also
// preserves callback-context narrowing even when an explicit type argument is
// supplied.
// ---------------------------------------------------------------------------

// G1: return type propagates across every arg form (inferred + explicit).
expectTypeOf(
  workflowStep.do<string>('g1: 2-arg explicit', async () => 'ok')
).toEqualTypeOf<Promise<string>>();
// The `T extends Rpc.Serializable<T>` constraint preserves the inferred literal
// return type rather than widening it to `number`.
expectTypeOf(
  workflowStep.do('g1: 2-arg inferred literal', async () => 42)
).toEqualTypeOf<Promise<42>>();
expectTypeOf(
  workflowStep.do<string>(
    'g1: 3-arg static explicit',
    {retries: {limit: 1, delay: 0}},
    async () => 'ok'
  )
).toEqualTypeOf<Promise<string>>();
expectTypeOf(
  workflowStep.do(
    'g1: 3-arg dynamic inferred',
    {retries: {limit: 1, delay: (): WorkflowDelayDuration => 5}},
    async (): Promise<string> => 'ok'
  )
).toEqualTypeOf<Promise<string>>();
expectTypeOf(
  workflowStep.do<string>(
    'g1: 3-arg dynamic explicit',
    {retries: {limit: 1, delay: (): WorkflowDelayDuration => 5}},
    async () => 'ok'
  )
).toEqualTypeOf<Promise<string>>();
expectTypeOf(
  workflowStep.do<string>(
    'g1: 4-arg config + rollback explicit',
    {retries: {limit: 1, delay: 0}},
    async () => 'ok',
    {rollback: async () => {}}
  )
).toEqualTypeOf<Promise<string>>();
expectTypeOf(
  workflowStep.do('g1: object return', async () => ({foo: 'x'}))
).toEqualTypeOf<Promise<{foo: string}>>();
expectTypeOf(
  workflowStep.do('g1: union return', async (): Promise<string | number> => 1)
).toEqualTypeOf<Promise<string | number>>();
expectTypeOf(
  workflowStep.do('g1: void return', async (): Promise<void> => {})
).toEqualTypeOf<Promise<void>>();
expectTypeOf(
  workflowStep.do('g1: array return', async (): Promise<string[]> => [])
).toEqualTypeOf<Promise<string[]>>();

// G2: a static delay is surfaced (present) in the callback context, and this
// holds even when the caller supplies an explicit type argument (the case that
// previously regressed to a spurious type error).
workflowStep.do<string>(
  'g2: explicit static number delay present',
  {retries: {limit: 1, delay: 0}},
  async (ctx) => {
    expectTypeOf(ctx.config.retries?.delay).toEqualTypeOf<
      WorkflowDelayDuration | number | undefined
    >();
    return 'ok';
  }
);
workflowStep.do<string>(
  'g2: explicit static duration delay present',
  {retries: {limit: 1, delay: '2 hours'}},
  async (ctx) => {
    expectTypeOf(ctx.config.retries?.delay).toEqualTypeOf<
      WorkflowDelayDuration | number | undefined
    >();
    return 'ok';
  }
);
workflowStep.do(
  'g2: inferred static delay with backoff',
  {retries: {limit: 1, delay: 0, backoff: 'exponential'}},
  async (ctx): Promise<string> => {
    expectTypeOf(ctx.config.retries?.backoff).toEqualTypeOf<
      WorkflowBackoff | undefined
    >();
    expectTypeOf(ctx.config.retries?.delay).toEqualTypeOf<
      WorkflowDelayDuration | number | undefined
    >();
    return 'ok';
  }
);

// G3: a delay function omits the resolved delay from the callback context,
// including under an explicit type argument and for async delay functions.
workflowStep.do<string>(
  'g3: explicit dynamic delay absent',
  {retries: {limit: 1, delay: (): WorkflowDelayDuration => '1 minute'}},
  async (ctx) => {
    expectTypeOf(ctx.config.retries).toEqualTypeOf<
      {limit: number; backoff?: WorkflowBackoff} | undefined
    >();
    // @ts-expect-error delay is absent under an explicit type argument too
    ctx.config.retries?.delay;
    return 'ok';
  }
);
workflowStep.do<string>(
  'g3: explicit async dynamic delay absent',
  {retries: {limit: 1, delay: async (): Promise<WorkflowDelayDuration> => 5}},
  async (ctx) => {
    expectTypeOf(ctx.config.retries).toEqualTypeOf<
      {limit: number; backoff?: WorkflowBackoff} | undefined
    >();
    return 'ok';
  }
);
workflowStep.do(
  'g3: inferred async dynamic delay absent',
  {retries: {limit: 1, delay: async (): Promise<WorkflowDelayDuration> => 5}},
  async (ctx): Promise<string> => {
    expectTypeOf(ctx.config.retries).toEqualTypeOf<
      {limit: number; backoff?: WorkflowBackoff} | undefined
    >();
    // @ts-expect-error delay is absent for an async delay function
    ctx.config.retries?.delay;
    return 'ok';
  }
);
workflowStep.do(
  'g3: dynamic delay with backoff, delay absent',
  {retries: {limit: 1, delay: (): WorkflowDelayDuration => 5, backoff: 'linear'}},
  async (ctx): Promise<string> => {
    expectTypeOf(ctx.config.retries?.backoff).toEqualTypeOf<
      WorkflowBackoff | undefined
    >();
    // @ts-expect-error delay is absent even when backoff is present
    ctx.config.retries?.delay;
    return 'ok';
  }
);

// G4: a config typed broadly (e.g. built separately) resolves to the fallback
// overload; it still compiles and exposes the non-narrowed (static) context.
declare const broadConfig: WorkflowStepConfig;
expectTypeOf(
  workflowStep.do('g4: prebuilt broad config', broadConfig, async (ctx) => {
    expectTypeOf(ctx.config.retries?.delay).toEqualTypeOf<
      WorkflowDelayDuration | number | undefined
    >();
    return 'ok';
  })
).toMatchTypeOf<Promise<string>>();
expectTypeOf(
  workflowStep.do<string>('g4: prebuilt broad config explicit', broadConfig, async () => 'ok')
).toEqualTypeOf<Promise<string>>();
declare const unionDelay:
  | WorkflowDelayDuration
  | number
  | WorkflowDelayFunction;
expectTypeOf(
  workflowStep.do(
    'g4: union-typed delay falls back',
    {retries: {limit: 1, delay: unionDelay}},
    async (): Promise<string> => 'ok'
  )
).toMatchTypeOf<Promise<string>>();

// G5: minimal / retries-less configs.
workflowStep.do(
  'g5: timeout only',
  {timeout: '10 seconds'},
  async (ctx): Promise<string> => {
    expectTypeOf(ctx.config.timeout).toMatchTypeOf<string | number | undefined>();
    return 'ok';
  }
);
workflowStep.do('g5: numeric timeout', {timeout: 5000}, async (): Promise<string> => 'ok');
workflowStep.do('g5: empty config', {}, async (): Promise<string> => 'ok');
workflowStep.do(
  'g5: sensitive only',
  {sensitive: 'output'},
  async (ctx): Promise<string> => {
    expectTypeOf(ctx.config.sensitive).toEqualTypeOf<
      WorkflowStepSensitivity | undefined
    >();
    return 'ok';
  }
);
workflowStep.do(
  'g5: full config',
  {retries: {limit: 1, delay: 0}, timeout: '1 minute', sensitive: 'output'},
  async (ctx): Promise<string> => {
    expectTypeOf(ctx.config.retries?.delay).toEqualTypeOf<
      WorkflowDelayDuration | number | undefined
    >();
    return 'ok';
  }
);

// G6: rollback combinations.
workflowStep.do(
  'g6: config + rollbackConfig delay function',
  {retries: {limit: 1, delay: 0}},
  async (): Promise<string> => 'ok',
  {rollback: async () => {}, rollbackConfig: {retries: {limit: 0, delay: () => 0}}}
);
workflowStep.do('g6: rollback output non-string', async (): Promise<{foo: string}> => ({foo: 'x'}), {
  rollback: async (rollbackCtx) => {
    expectTypeOf(rollbackCtx.output).toEqualTypeOf<{foo: string} | undefined>();
  },
});
workflowStep.do<string>(
  'g6: explicit + config + rollback options',
  {retries: {limit: 1, delay: 0}},
  async () => 'ok',
  {rollback: async () => {}, rollbackConfig: {retries: {limit: 0, delay: 0}}}
);

// G7: negatives.
// @ts-expect-error delay must not be a boolean
workflowStep.do('g7: delay boolean', {retries: {limit: 1, delay: true}}, async () => 'ok');
// @ts-expect-error delay must not be an object
workflowStep.do('g7: delay object', {retries: {limit: 1, delay: {}}}, async () => 'ok');
// @ts-expect-error retries requires a limit
workflowStep.do('g7: retries no limit', {retries: {delay: 0}}, async () => 'ok');
// @ts-expect-error retries requires a delay
workflowStep.do('g7: retries no delay', {retries: {limit: 1}}, async () => 'ok');
// @ts-expect-error timeout must not be a boolean
workflowStep.do('g7: timeout boolean', {timeout: true}, async () => 'ok');
// @ts-expect-error sensitive must be the 'output' literal
workflowStep.do('g7: sensitive wrong', {sensitive: 'nope'}, async () => 'ok');
// @ts-expect-error a sync delay function must return a delay duration
workflowStep.do('g7: delay fn wrong return', {retries: {limit: 1, delay: (): boolean => true}}, async () => 'ok');
// @ts-expect-error an async delay function must resolve to a delay duration
workflowStep.do('g7: async delay fn wrong return', {retries: {limit: 1, delay: async (): Promise<boolean> => true}}, async () => 'ok');
// @ts-expect-error each overload declares a single type parameter
workflowStep.do<string, WorkflowStepConfig>('g7: too many type args', {retries: {limit: 1, delay: 0}}, async () => 'ok');
// @ts-expect-error the callback must return a Promise
workflowStep.do('g7: callback not async', {retries: {limit: 1, delay: 0}}, () => 'ok');
// @ts-expect-error the step name must be a string
workflowStep.do(123, async (): Promise<string> => 'ok');
// @ts-expect-error unknown config keys are rejected
workflowStep.do('g7: excess config key', {retries: {limit: 1, delay: 0}, bogus: 1}, async () => 'ok');

// G8: standalone delay functions may return any accepted delay duration shape.
const delayReturningNumber: WorkflowDelayFunction = () => 5;
void delayReturningNumber;
const delayReturningDuration: WorkflowDelayFunction = () => '1 minute';
void delayReturningDuration;
const asyncDelay: WorkflowDelayFunction = async () => 5;
void asyncDelay;

// ---------------------------------------------------------------------------
// Rollback context delay narrowing.
//
// The rollback handler receives the step context, so it mirrors the same delay
// discriminant as the step callback: a static delay surfaces the resolved
// `config.retries.delay`, while a delay function omits it. This holds under an
// explicit return-type argument too. `rollbackConfig` continues to accept a
// delay function of its own.
// ---------------------------------------------------------------------------

// A static-delay step exposes the resolved delay in the rollback context.
workflowStep.do(
  'rollback ctx static delay present',
  {retries: {limit: 1, delay: 0}},
  async (): Promise<string> => 'ok',
  {
    rollback: async (rollbackCtx) => {
      expectTypeOf(rollbackCtx.ctx.config.retries?.delay).toEqualTypeOf<
        WorkflowDelayDuration | number | undefined
      >();
    },
  }
);

// A delay-function step omits the resolved delay in the rollback context.
workflowStep.do(
  'rollback ctx dynamic delay absent',
  {retries: {limit: 1, delay: (): WorkflowDelayDuration => '1 minute'}},
  async (): Promise<string> => 'ok',
  {
    rollback: async (rollbackCtx) => {
      expectTypeOf(rollbackCtx.ctx.config.retries).toEqualTypeOf<
        {limit: number; backoff?: WorkflowBackoff} | undefined
      >();
      // @ts-expect-error delay is absent when the step used a delay function
      rollbackCtx.ctx.config.retries?.delay;
    },
  }
);

// The same omission holds under an explicit return-type argument.
workflowStep.do<string>(
  'rollback ctx dynamic delay absent explicit',
  {retries: {limit: 1, delay: (): WorkflowDelayDuration => '1 minute'}},
  async () => 'ok',
  {
    rollback: async (rollbackCtx) => {
      expectTypeOf(rollbackCtx.ctx.config.retries).toEqualTypeOf<
        {limit: number; backoff?: WorkflowBackoff} | undefined
      >();
      // @ts-expect-error delay is absent under an explicit type argument too
      rollbackCtx.ctx.config.retries?.delay;
    },
  }
);

// An async delay-function step also omits the resolved delay.
workflowStep.do(
  'rollback ctx async dynamic delay absent',
  {retries: {limit: 1, delay: async (): Promise<WorkflowDelayDuration> => 5}},
  async (): Promise<string> => 'ok',
  {
    rollback: async (rollbackCtx) => {
      expectTypeOf(rollbackCtx.ctx.config.retries).toEqualTypeOf<
        {limit: number; backoff?: WorkflowBackoff} | undefined
      >();
    },
  }
);

// The no-config (2-arg) form keeps the default (static) rollback context.
workflowStep.do('rollback ctx default delay present', async (): Promise<string> => 'ok', {
  rollback: async (rollbackCtx) => {
    expectTypeOf(rollbackCtx.ctx.config.retries?.delay).toEqualTypeOf<
      WorkflowDelayDuration | number | undefined
    >();
  },
});

// `rollbackConfig` accepts an async delay function of its own.
workflowStep.do('rollback config async delay function', async (): Promise<string> => 'ok', {
  rollback: async () => {},
  rollbackConfig: {retries: {limit: 0, delay: async () => 5}},
});

// @ts-expect-error a rollbackConfig delay function must return a delay duration
workflowStep.do('rollback config delay wrong return', async () => 'ok', {
  rollback: async () => {},
  rollbackConfig: {retries: {limit: 0, delay: (): boolean => true}},
});

declare const cronSchedule: WorkflowCronSchedule;
expectTypeOf(cronSchedule.cron).toEqualTypeOf<string>();
expectTypeOf(cronSchedule.scheduledTime).toEqualTypeOf<number>();

type WorkflowPayload = {foo: string};
declare const workflowEvent: WorkflowEvent<WorkflowPayload>;
expectTypeOf(workflowEvent.payload).toEqualTypeOf<Readonly<WorkflowPayload>>();
expectTypeOf(workflowEvent.timestamp).toEqualTypeOf<Date>();
expectTypeOf(workflowEvent.instanceId).toEqualTypeOf<string>();
expectTypeOf(workflowEvent.workflowName).toEqualTypeOf<string>();
expectTypeOf(workflowEvent.schedule).toEqualTypeOf<
  WorkflowCronSchedule | undefined
>();

const withoutSchedule: WorkflowEvent<WorkflowPayload> = {
  payload: {foo: 'bar'},
  timestamp: new Date(),
  instanceId: 'abc',
  workflowName: 'my-workflow',
};
expectTypeOf(withoutSchedule.schedule).toEqualTypeOf<
  WorkflowCronSchedule | undefined
>();
