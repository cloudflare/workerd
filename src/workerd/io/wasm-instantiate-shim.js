// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// This file contains a shim for WebAssembly.Instance and WebAssembly.instantiate. Currently, the
// runtime does not support instantiateStreaming, but if this ever changes, we will need to add a
// shim for that too. V8's `SetWasmInstanceCallback` was considered as an alternative, but does
// not quite work since it runs BEFORE instantiation, when the operations we want to do must happen
// after.

(function (originalInstantiate, originalInstance, registerShutdown, wa) {
  // Find memory from exports or imports. Returns Memory instance or undefined.
  // When searching imports, only considers entries whose declared import kind is
  // 'memory' (via WebAssembly.Module.imports), so that a Memory passed as an
  // externref is not mistaken for the module's linear memory.
  function findMemory(instance, imports, module) {
    // First, check if memory is exported
    const importedMemory = wa.Module.imports(module).find(
      ({ kind }) => kind === 'memory'
    );
    if (importedMemory) {
      const value = imports[importedMemory.module][importedMemory.name];
      return value instanceof wa.Memory && value;
    }
    const exportedMemory = wa.Module.exports(module).find(
      ({ kind }) => kind === 'memory'
    );
    if (exportedMemory) return instance.exports[exportedMemory.name];
    return undefined;
  }

  function checkAndRegisterShutdown(instance, imports, module) {
    const exports = instance.exports;
    if (!exports) return;
    const terminatedGlobal = exports['__instance_terminated'];
    // __instance_terminated is required; __instance_signal is optional.
    if (!(terminatedGlobal instanceof wa.Global)) return;
    const signalGlobal = exports['__instance_signal'];
    const hasSignal = signalGlobal instanceof wa.Global;
    const memory = findMemory(instance, imports, module);
    if (memory) {
      // Pass -1 as the signal offset when __instance_signal is not exported.
      // The C++ side interprets -1 as "no signal address".
      registerShutdown(
        memory,
        hasSignal ? signalGlobal.value : -1,
        terminatedGlobal.value
      );
    }
  }

  // WebAssembly.instantiate has two overloads:
  //   instantiate(bytes, imports?, compileOptions?) -> Promise<{module, instance}>
  //   instantiate(module, imports?, compileOptions?) -> Promise<Instance>
  // Use apply to forward all arguments so compileOptions (and any future args) are not dropped.
  wa.instantiate = function instantiate() {
    var args = arguments;
    return originalInstantiate.apply(wa, args).then(function (result) {
      // Called with bytes: result is {module, instance}.
      // Called with a Module: result is just the Instance.
      var instance = result.instance || result;
      var module = result.module || args[0];
      checkAndRegisterShutdown(instance, args[1], module);
      return result;
    });
  };

  // new WebAssembly.Instance(module, imports?)
  // Forward all arguments and new.target so subclassing works correctly.
  wa.Instance = function Instance() {
    var instance = Reflect.construct(
      originalInstance,
      arguments,
      new.target || originalInstance
    );
    checkAndRegisterShutdown(instance, arguments[1], arguments[0]);
    return instance;
  };
  // Point the shim's prototype at the original so instanceof checks continue to work.
  wa.Instance.prototype = originalInstance.prototype;
  Object.defineProperty(wa.Instance.prototype, 'constructor', {
    value: wa.Instance,
    writable: true,
    configurable: true,
  });
});
