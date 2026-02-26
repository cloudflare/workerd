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

  function registerExports(instance, imports, module) {
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
        const instance = result.instance || result;
        const module = result.module || moduleOrBytes;
        registerExports(instance, imports, module);
        return result;
      });
  };

  wa.Instance = function Instance(module, imports) {
    const instance = new originalInstance(module, imports);
    registerExports(instance, imports, module);
    return instance;
  };
  wa.Instance.prototype = originalInstance.prototype;
  Object.defineProperty(wa.Instance.prototype, 'constructor', {
    value: wa.Instance,
    writable: true,
    configurable: true,
  });
});
