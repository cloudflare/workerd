// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// This file contains a shim for WebAssembly.Instance and WebAssembly.instantiate. Currently, the
// runtime does not support instantiateStreaming, but if this ever changes, we will need to add a
// shim for that too. V8's `SetWasmInstanceCallback` was considered as an alternative, but does
// not quite work since it runs BEFORE instantiation, when the operations we want to do must happen
// after.

(function (registerShutdown) {
  const {
    Instance: originalInstance,
    instantiate: originalInstantiate,
    Module,
    Memory,
    Global,
  } = WebAssembly;

  // Finds the first imported or exported memory with type kind 'memory'.
  function findMemory(instance, importObj, module) {
    const importedMemory = Module.imports(module).find(
      ({ kind }) => kind === 'memory'
    );
    if (importedMemory) {
      // instantiation was already successful => importObj is known to
      // have the right shape, and checkAndRegisterShutdown is wrapped
      // in try-catch in case this fails anyway.
      const value = importObj[importedMemory.module][importedMemory.name];
      return value instanceof Memory ? value : undefined;
    }
    const exportedMemory = Module.exports(module).find(
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

  // WebAssembly.instantiate has two overloads:
  //   instantiate(bytes, imports?, compileOptions?) -> Promise<{module, instance}>
  //   instantiate(module, imports?, compileOptions?) -> Promise<Instance>
  // NOTE: Exactly 1 argument defined to match WebAssembly.instantiate length
  WebAssembly.instantiate = function instantiate(moduleOrBytes) {
    const importObj = arguments[1];
    return Reflect.apply(originalInstantiate, this, arguments).then(
      function (result) {
        // never fail instantiation for failed registration
        try {
          // Called with bytes: result is {module, instance}.
          // Called with a Module: result is just the Instance.
          const instance = result.instance || result;
          const module = result.module || moduleOrBytes;
          checkAndRegisterShutdown(instance, importObj, module);
        } catch {}
        return result;
      }
    );
  };

  // new WebAssembly.Instance(module, imports?)
  // Forward all arguments and new.target so subclassing works correctly.
  // NOTE: Exactly 1 argument defined to match WebAssembly.Instance length
  function Instance(module) {
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
  // Point the shim's prototype at the original so instanceof checks continue to work.
  Instance.prototype = originalInstance.prototype;
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
});
