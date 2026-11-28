// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// This file contains a shim for WebAssembly.Instance, WebAssembly.instantiate and
// instantiateStreaming that registers instances exporting the shutdown signal. V8's
// `SetWasmInstanceCallback` was considered as an alternative, but does not quite work since it
// runs BEFORE instantiation, when the operations we want to do must happen after.
//
// The asynchronous functions (compile, instantiate, compileStreaming and instantiateStreaming) call
// V8 through compileWasm(), the embedder's, which settles their promises in the current request
// and, under webassembly_dynamic_compilation, hands their bytes to the embedder; see
// Worker::Api::newWasmCompileFunction(). Without one, they call V8 directly.

(function (registerShutdown, compileWasm) {
  const { apply: ReflectApply } = Reflect;
  const {
    getOwnPropertyDescriptor: ObjectGetOwnPropertyDescriptor,
    getPrototypeOf: ObjectGetPrototypeOf,
  } = Object;
  const { toStringTag: SymbolToStringTag } = Symbol;
  // A bound call() doesn't consult Function.prototype when it runs.
  const uncurry = (fn) => Function.prototype.call.bind(fn);
  const PromiseResolve = Promise.resolve.bind(Promise);
  const PromisePrototypeThen = uncurry(Promise.prototype.then);
  const StringPrototypeSlice = uncurry(String.prototype.slice);
  const StringPrototypeToLowerCase = uncurry(String.prototype.toLowerCase);
  const TypeErrorConstructor = TypeError;
  if (compileWasm === undefined) {
    compileWasm = function (original, thisArg, args) {
      return ReflectApply(original, thisArg, args);
    };
  }

  const {
    Instance: originalInstance,
    instantiate: originalInstantiate,
    compile: originalCompile,
    Module: originalModule,
    Memory,
    Global,
  } = WebAssembly;

  const { imports: ModuleImports, exports: ModuleExports } = originalModule;

  // Finds the first imported or exported memory with type kind 'memory'.
  function findMemory(instance, importObj, module) {
    const importedMemory = ModuleImports(module).find(
      ({ kind }) => kind === 'memory'
    );
    if (importedMemory) {
      // instantiation was already successful => importObj is known to
      // have the right shape, and checkAndRegisterShutdown is wrapped
      // in try-catch in case this fails anyway.
      const value = importObj[importedMemory.module][importedMemory.name];
      return value instanceof Memory ? value : undefined;
    }
    const exportedMemory = ModuleExports(module).find(
      ({ kind }) => kind === 'memory'
    );
    if (exportedMemory) return instance.exports[exportedMemory.name];
    return undefined;
  }

  function checkAndRegisterShutdown(instance, imports, module) {
    const exports = instance.exports;
    if (!exports) return;
    const terminatedGlobal = exports['__instance_terminated'];
    const signalGlobal = exports['__instance_signal'];
    const hasTerminated = terminatedGlobal instanceof Global;
    const hasSignal = signalGlobal instanceof Global;
    // Register if at least one is present.
    if (!hasTerminated && !hasSignal) return;
    const memory = findMemory(instance, imports, module);
    if (memory) {
      // Pass -1 for whichever offset is absent. The C++ side interprets -1 as "no address".
      registerShutdown(
        instance,
        memory,
        hasSignal ? signalGlobal.value : -1,
        hasTerminated ? terminatedGlobal.value : -1
      );
    }
  }

  // The arguments V8's WebAssembly functions read, which treat a missing one as undefined. An
  // array literal doesn't consult Array.prototype, which the Worker may change.
  const argumentArray = (args) => [args[0], args[1], args[2]];

  // Registers the instance that `promise` resolves with for the shutdown signal, and resolves with
  // what it does. `promise` resolves with {module, instance}, or, if instantiate() was called with
  // `module`, the Instance.
  function registerResult(promise, imports, module) {
    return PromisePrototypeThen(promise, (result) => {
      // never fail instantiation for failed registration
      try {
        checkAndRegisterShutdown(
          result.instance || result,
          imports,
          result.module || module
        );
      } catch {}
      return result;
    });
  }

  // WebAssembly.instantiate has two overloads:
  //   instantiate(bytes, imports?, compileOptions?) -> Promise<{module, instance}>
  //   instantiate(module, imports?, compileOptions?) -> Promise<Instance>
  // NOTE: Exactly 1 argument defined to match WebAssembly.instantiate length
  WebAssembly.instantiate = function instantiate(moduleOrBytes) {
    return registerResult(
      compileWasm(originalInstantiate, this, argumentArray(arguments)),
      arguments[1],
      moduleOrBytes
    );
  };

  // NOTE: Exactly 1 argument defined to match WebAssembly.compile length
  WebAssembly.compile = function compile(bytes) {
    return compileWasm(originalCompile, this, argumentArray(arguments));
  };

  // compileStreaming() and instantiateStreaming() exist only under the webassembly_streaming
  // compatibility flag.
  const {
    compileStreaming: originalCompileStreaming,
    instantiateStreaming: originalInstantiateStreaming,
  } = WebAssembly;
  if (originalCompileStreaming !== undefined) {
    const readBody = prepareStreaming();
    // V8 resolves its first argument, then calls the isolate's WasmStreamingCallback with the
    // result, which here is a function that reads the Response's body into V8's compiler.
    const streamingArguments = (args) => [
      PromisePrototypeThen(PromiseResolve(args[0]), readBody),
      args[1],
      args[2],
    ];
    // NOTE: Exactly 1 argument defined to match WebAssembly.compileStreaming length
    WebAssembly.compileStreaming = function compileStreaming(source) {
      return compileWasm(
        originalCompileStreaming,
        this,
        streamingArguments(arguments)
      );
    };
    // NOTE: Exactly 1 argument defined to match WebAssembly.instantiateStreaming length
    WebAssembly.instantiateStreaming = function instantiateStreaming(source) {
      return registerResult(
        compileWasm(
          originalInstantiateStreaming,
          this,
          streamingArguments(arguments)
        ),
        arguments[1]
      );
    };
  }

  // new WebAssembly.Instance(module, imports?)
  // Forward all arguments and new.target so subclassing works correctly.
  // NOTE: Exactly 1 argument defined to match WebAssembly.Instance length
  function Instance(module) {
    // Like the native constructor, throw when called without `new`.
    if (new.target === undefined) {
      throw new TypeError("WebAssembly.Instance must be invoked with 'new'");
    }
    const instance = Reflect.construct(
      originalInstance,
      arguments,
      new.target || originalInstance
    );
    // never fail instantiation for failed registration
    try {
      checkAndRegisterShutdown(instance, arguments[1], module);
    } catch {}
    return instance;
  }
  // Point the shim's prototype at the original so instanceof checks continue to work. Like the
  // native constructor's, it is read-only.
  Object.defineProperty(Instance, 'prototype', {
    value: originalInstance.prototype,
    writable: false,
  });
  Object.defineProperty(Instance.prototype, 'constructor', {
    value: Instance,
    writable: true,
    configurable: true,
  });
  Object.defineProperty(WebAssembly, 'Instance', {
    value: Instance,
    writable: true,
    configurable: true,
  });

  // new WebAssembly.Module(bytes, compileOptions?)
  // V8 calls the isolate's WasmModuleCallback, which under webassembly_dynamic_compilation hands
  // the bytes to the embedder, before it checks that it was called with `new`, so this checks
  // first.
  // NOTE: Exactly 1 argument defined to match WebAssembly.Module length
  function Module(bytes) {
    if (new.target === undefined) {
      throw new TypeError("WebAssembly.Module must be invoked with 'new'");
    }
    return Reflect.construct(originalModule, arguments, new.target);
  }
  // Like the native constructor, it inherits from AbstractModuleSource, its prototype is
  // read-only, and its static methods are writable, configurable and not enumerable.
  Object.setPrototypeOf(Module, ObjectGetPrototypeOf(originalModule));
  Object.defineProperty(Module, 'prototype', {
    value: originalModule.prototype,
    writable: false,
  });
  Object.defineProperty(Module.prototype, 'constructor', {
    value: Module,
    writable: true,
    configurable: true,
  });
  for (const name of ['imports', 'exports', 'customSections']) {
    Object.defineProperty(
      Module,
      name,
      ObjectGetOwnPropertyDescriptor(originalModule, name)
    );
  }
  Object.defineProperty(WebAssembly, 'Module', {
    value: Module,
    writable: true,
    configurable: true,
  });

  // Captures what readBody() uses while only workerd's code has run, so that the Worker can't
  // change it later.
  function prepareStreaming() {
    // Reads `name` with the getter that `object` or the first of its prototypes with `name` had
    // at startup, or, if none has `name` (JSG defines some properties on each instance), from the
    // object it is called with.
    const getter = (object, name) => {
      for (; object !== null; object = ObjectGetPrototypeOf(object)) {
        const descriptor = ObjectGetOwnPropertyDescriptor(object, name);
        if (descriptor?.get !== undefined) {
          return uncurry(descriptor.get);
        }
      }
      return (thisArg) => thisArg[name];
    };
    const getHeaders = getter(Response.prototype, 'headers');
    const getType = getter(Response.prototype, 'type');
    const getStatus = getter(Response.prototype, 'status');
    const getUrl = getter(Response.prototype, 'url');
    const getBody = getter(Response.prototype, 'body');
    const getBodyUsed = getter(Response.prototype, 'bodyUsed');
    const HeadersPrototypeGet = uncurry(Headers.prototype.get);
    const getLocked = getter(ReadableStream.prototype, 'locked');
    const ReadableStreamPrototypeGetReader = uncurry(
      ReadableStream.prototype.getReader
    );
    const ReadableStreamDefaultReaderPrototypeRead = uncurry(
      ReadableStreamDefaultReader.prototype.read
    );
    const getTypedArrayTag = getter(
      ObjectGetPrototypeOf(Uint8Array.prototype),
      SymbolToStringTag
    );
    const responseTypes = {
      __proto__: null,
      basic: true,
      cors: true,
      default: true,
      error: true,
      opaque: true,
      opaqueredirect: true,
    };

    // Implements "compile a potential WebAssembly response" from the WebAssembly Web API, apart
    // from the compiling, and returns the function that reads the body into a sink.
    return function readBody(response) {
      // Only a Response has one of these types. This doesn't compare with the Response
      // constructor, since a Worker started from a startup snapshot has a different one from the
      // one here. A fake that passes reaches only the native Headers and ReadableStream methods
      // above, which check what they are called on.
      let type;
      try {
        type = getType(response);
      } catch {}
      if (typeof type !== 'string' || responseTypes[type] !== true) {
        throw new TypeErrorConstructor(
          'WebAssembly.compileStreaming() and WebAssembly.instantiateStreaming() expect a Response.'
        );
      }
      const contentType = HeadersPrototypeGet(
        getHeaders(response),
        'content-type'
      );
      if (contentType === null) {
        throw new TypeErrorConstructor(
          'The Response for WebAssembly streaming compilation has no Content-Type.'
        );
      }
      // Strip HTTP tab or space bytes from both ends, then compare byte-case-insensitively.
      const isTabOrSpace = (c) => c === ' ' || c === '\t';
      let start = 0;
      let end = contentType.length;
      while (start < end && isTabOrSpace(contentType[start])) start++;
      while (end > start && isTabOrSpace(contentType[end - 1])) end--;
      const mimeType = StringPrototypeToLowerCase(
        StringPrototypeSlice(contentType, start, end)
      );
      if (mimeType !== 'application/wasm') {
        throw new TypeErrorConstructor(
          `The Response for WebAssembly streaming compilation has the Content-Type "${contentType}", not "application/wasm".`
        );
      }
      if (type === 'error') {
        throw new TypeErrorConstructor(
          "WebAssembly streaming compilation can't use a network error Response."
        );
      }
      const status = getStatus(response);
      if (status < 200 || status > 299) {
        throw new TypeErrorConstructor(
          `The Response for WebAssembly streaming compilation has the status ${status}, which isn't ok.`
        );
      }
      if (getBodyUsed(response)) {
        throw new TypeErrorConstructor(
          'The Response body for WebAssembly streaming compilation has already been used.'
        );
      }
      const body = getBody(response);
      if (body !== null && getLocked(body)) {
        throw new TypeErrorConstructor(
          'The Response body for WebAssembly streaming compilation is locked.'
        );
      }
      const url = getUrl(response);

      return async function (sink) {
        if (url !== '') sink.setUrl(url);
        try {
          // A Response without a body compiles no bytes, which V8 rejects as invalid code.
          if (body !== null) {
            const reader = ReadableStreamPrototypeGetReader(body);
            for (;;) {
              const { done, value } =
                await ReadableStreamDefaultReaderPrototypeRead(reader);
              if (done) break;
              if (getTypedArrayTag(value) !== 'Uint8Array') {
                throw new TypeErrorConstructor(
                  'A chunk of the Response body for WebAssembly streaming compilation is not a Uint8Array.'
                );
              }
              sink.push(value);
            }
          }
          sink.finish();
        } catch (e) {
          sink.abort(e);
        }
      };
    };
  }
});
