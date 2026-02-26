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
    const memory = instance.exports['memory'];
    if (memory instanceof wa.Memory) return memory;
    // Otherwise, check the module's declared memory imports
    if (imports && module) {
      const descs = wa.Module.imports(module);
      for (let i = 0; i < descs.length; i++) {
        if (descs[i].kind === 'memory') {
          const ns = imports[descs[i].module];
          if (ns) {
            const mem = ns[descs[i].name];
            if (mem instanceof wa.Memory) return mem;
          }
        }
      }
    }
    return undefined;
  }

  function checkAndRegisterShutdown(instance, imports, module) {
    const exports = instance.exports;
    const signalGlobal = exports['__instance_signal'];
    const terminatedGlobal = exports['__instance_terminated'];
    if (
      signalGlobal instanceof wa.Global &&
      terminatedGlobal instanceof wa.Global
    ) {
      const memory = findMemory(instance, imports, module);
      if (memory) {
        registerShutdown(memory, signalGlobal.value, terminatedGlobal.value);
      }
    }
  }

  wa.instantiate = function instantiate(moduleOrBytes, imports) {
    return originalInstantiate
      .call(wa, moduleOrBytes, imports)
      .then(function (result) {
        // WebAssembly.instantiate has two overloads: called with bytes it resolves to
        // { module, instance }, called with a Module it resolves to just the Instance.
        const instance = result.instance || result;
        const module = result.module || moduleOrBytes;
        checkAndRegisterShutdown(instance, imports, module);
        return result;
      });
  };

  wa.Instance = function Instance(module, imports) {
    const instance = new originalInstance(module, imports);
    checkAndRegisterShutdown(instance, imports, module);
    return instance;
  };
  wa.Instance = function Instance(module) {
    const instance = Reflect.construct(originalInstance, arguments);
    checkAndRegisterShutdown(instance, arguments[1], module);
    return instance;
  };
  Object.defineProperty(wa.Instance.prototype, 'constructor', {
    value: wa.Instance,
    writable: true,
    configurable: true,
  });
});
