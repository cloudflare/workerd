// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// This file contains shims for request-time WebAssembly compilation. Streaming compilation remains
// unavailable because it cannot pass through byte retention before compilation.

(function (prepareWasm, prepareWasmInBackground) {
  const {
    compile: originalCompile,
    Instance,
    instantiate: originalInstantiate,
    Module,
  } = WebAssembly;
  // Capture trusted intrinsics once so Worker code cannot alter upload ordering or conversions.
  const reflectApply = Reflect.apply;
  const reflectConstruct = Reflect.construct;
  const arrayFrom = Array.from;
  const stringConstructor = String;
  const promiseThen = Promise.prototype.then;
  const rejectPromise = Promise.reject.bind(Promise);

  function then(promise, onFulfilled) {
    return reflectApply(promiseThen, promise, [onFulfilled]);
  }

  function copyCompileOptions(options) {
    if (options === undefined) return undefined;
    const result = {};
    const builtins = options.builtins;
    if (builtins !== undefined) {
      result.builtins = reflectApply(arrayFrom, undefined, [
        builtins,
        stringConstructor,
      ]);
    }
    const importedStringConstants = options.importedStringConstants;
    if (importedStringConstants !== undefined) {
      result.importedStringConstants = stringConstructor(
        importedStringConstants
      );
    }
    return result;
  }

  WebAssembly.compile = new Proxy(originalCompile, {
    apply(_, receiver, args) {
      try {
        const [snapshot, upload] = prepareWasm(args[0]);
        args[0] = snapshot;
        if (args.length > 1) args[1] = copyCompileOptions(args[1]);
        return then(upload, () => reflectConstruct(Module, args));
      } catch (error) {
        return rejectPromise(error);
      }
    },
  });

  WebAssembly.instantiate = new Proxy(originalInstantiate, {
    apply(_, receiver, args) {
      const moduleOrBytes = args[0];
      if (moduleOrBytes instanceof Module) {
        return reflectApply(originalInstantiate, receiver, args);
      }

      let start;
      try {
        const [snapshot, upload] = prepareWasm(moduleOrBytes);
        if (args.length > 2) args[2] = copyCompileOptions(args[2]);
        start = then(upload, () => {
          const moduleArgs = args.length > 2 ? [snapshot, args[2]] : [snapshot];
          const module = reflectConstruct(Module, moduleArgs);
          const instance = reflectConstruct(Instance, [module, args[1]]);
          return { module, instance };
        });
      } catch (error) {
        return rejectPromise(error);
      }
      return start;
    },
  });

  // WebAssembly.Module is synchronous, so its upload is retained as a background task.
  const ModuleWrapper = new Proxy(Module, {
    construct(target, args, newTarget) {
      args[0] = prepareWasmInBackground(args[0]);
      return reflectConstruct(target, args, newTarget);
    },
  });
  Object.defineProperty(ModuleWrapper.prototype, 'constructor', {
    value: ModuleWrapper,
    writable: true,
    configurable: true,
  });
  Object.defineProperty(WebAssembly, 'Module', {
    value: ModuleWrapper,
    writable: true,
    configurable: true,
  });
});
