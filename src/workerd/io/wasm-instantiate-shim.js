(function (originalInstantiate, originalInstance, registerShutdown, wa) {
  // Find memory from exports or imports. Returns Memory instance or undefined.
  // When searching imports, only considers entries whose declared import kind is
  // 'memory' (via WebAssembly.Module.imports), so that a Memory passed as an
  // externref is not mistaken for the module's linear memory.
  function findMemory(instance, imports, module) {
    // First, check if memory is exported
    var memory = instance.exports['memory'];
    if (memory instanceof wa.Memory) return memory;
    // Otherwise, check the module's declared memory imports
    if (imports && module) {
      var descs = wa.Module.imports(module);
      for (var i = 0; i < descs.length; i++) {
        if (descs[i].kind === 'memory') {
          var ns = imports[descs[i].module];
          if (ns) {
            var mem = ns[descs[i].name];
            if (mem instanceof wa.Memory) return mem;
          }
        }
      }
    }
    return undefined;
  }

  function checkExports(instance, imports, module) {
    var exports = instance.exports;
    var signalGlobal = exports['__signal_address'];
    var terminatedGlobal = exports['__terminated_address'];
    if (
      signalGlobal instanceof wa.Global &&
      terminatedGlobal instanceof wa.Global
    ) {
      var memory = findMemory(instance, imports, module);
      if (memory) {
        registerShutdown(memory, signalGlobal.value, terminatedGlobal.value);
      }
    }
  }

  wa.instantiate = function instantiate(moduleOrBytes, imports) {
    return originalInstantiate
      .call(wa, moduleOrBytes, imports)
      .then(function (result) {
        var instance = result.instance || result;
        var module = result.module || moduleOrBytes;
        checkExports(instance, imports, module);
        return result;
      });
  };

  wa.Instance = function Instance(module, imports) {
    var instance = new originalInstance(module, imports);
    checkExports(instance, imports, module);
    return instance;
  };
  wa.Instance.prototype = originalInstance.prototype;
  Object.defineProperty(wa.Instance.prototype, 'constructor', {
    value: wa.Instance,
    writable: true,
    configurable: true,
  });
});
