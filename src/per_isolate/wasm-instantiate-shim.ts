'use strict';

// Installs a shim around WebAssembly.instantiate and WebAssembly.Instance that
// registers Wasm instances exporting __instance_signal / __instance_terminated
// for CPU shutdown signaling.
//
// Kept in per_isolate/ so it ships with a build-time compile cache and is
// pre-evaluated (eagerly compiled) inside process-wide baseline snapshots and
// carried through ephemeral zygote snapshots rather than compiled from source
// on every isolate startup.

const { ObjectDefineProperty, ObjectFreeze, TypeError } = primordials;

type RegisterShutdownFn = (
  instance: WebAssembly.Instance,
  memory: WebAssembly.Memory,
  signalOffset: number,
  terminatedOffset: number
) => void;

function install(
  registerShutdown: RegisterShutdownFn,
  wasm: typeof WebAssembly
): void {
  const {
    Instance: originalInstance,
    instantiate: originalInstantiate,
    Module,
    Memory,
    Global,
  } = wasm;

  // Finds the first imported or exported memory with type kind 'memory'.
  function findMemory(
    instance: WebAssembly.Instance,
    importObj: WebAssembly.Imports | undefined,
    module: WebAssembly.Module
  ): WebAssembly.Memory | undefined {
    const importedMemory = Module.imports(module).find(
      ({ kind }) => kind === 'memory'
    );
    if (importedMemory && importObj !== undefined) {
      // instantiation was already successful => importObj is known to
      // have the right shape, and checkAndRegisterShutdown is wrapped
      // in try-catch in case this fails anyway.
      const ns = importObj[importedMemory.module];
      const value = ns !== undefined ? ns[importedMemory.name] : undefined;
      return value instanceof Memory ? value : undefined;
    }
    const exportedMemory = Module.exports(module).find(
      ({ kind }) => kind === 'memory'
    );
    if (exportedMemory) {
      const value = instance.exports[exportedMemory.name];
      return value instanceof Memory ? value : undefined;
    }
    return undefined;
  }

  function checkAndRegisterShutdown(
    instance: WebAssembly.Instance,
    imports: WebAssembly.Imports | undefined,
    module: WebAssembly.Module
  ): void {
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
      // Pass -1 for whichever offset is absent. The C++ side interprets -1 as
      // "no address".
      registerShutdown(
        instance,
        memory,
        hasSignal ? (signalGlobal.value as number) : -1,
        hasTerminated ? (terminatedGlobal.value as number) : -1
      );
    }
  }

  // WebAssembly.instantiate has two overloads:
  //   instantiate(bytes, imports?, compileOptions?) -> Promise<{module, instance}>
  //   instantiate(module, imports?, compileOptions?) -> Promise<Instance>
  // NOTE: Exactly 1 argument defined to match WebAssembly.instantiate length.
  wasm.instantiate = function instantiate(
    this: unknown,
    moduleOrBytes: WebAssembly.Module | BufferSource
  ): Promise<WebAssembly.WebAssemblyInstantiatedSource | WebAssembly.Instance> {
    const args = arguments;
    const importObj = args[1] as WebAssembly.Imports | undefined;
    return (
      Reflect.apply(
        originalInstantiate as (...a: unknown[]) => Promise<unknown>,
        this,
        args
      ) as Promise<
        WebAssembly.WebAssemblyInstantiatedSource | WebAssembly.Instance
      >
    ).then(function (result) {
      // Never fail instantiation for failed registration.
      try {
        // Called with bytes: result is {module, instance}.
        // Called with a Module: result is just the Instance.
        const asSource = result as Partial<WebAssembly.WebAssemblyInstantiatedSource>;
        const instance =
          asSource.instance ?? (result as WebAssembly.Instance);
        const module =
          asSource.module ?? (moduleOrBytes as WebAssembly.Module);
        checkAndRegisterShutdown(instance, importObj, module);
      } catch {
        // Intentionally ignored.
      }
      return result;
    });
  } as typeof WebAssembly.instantiate;

  // new WebAssembly.Instance(module, imports?)
  // Forward all arguments and new.target so subclassing works correctly.
  // NOTE: Exactly 1 argument defined to match WebAssembly.Instance length.
  function Instance(this: unknown, module: WebAssembly.Module): WebAssembly.Instance {
    // Like the native constructor, throw when called without `new`.
    if (new.target === undefined) {
      throw new TypeError("WebAssembly.Instance must be invoked with 'new'");
    }
    const args = arguments;
    const instance = Reflect.construct(
      originalInstance,
      args,
      new.target || originalInstance
    ) as WebAssembly.Instance;
    // Never fail instantiation for failed registration.
    try {
      checkAndRegisterShutdown(
        instance,
        args[1] as WebAssembly.Imports | undefined,
        module
      );
    } catch {
      // Intentionally ignored.
    }
    return instance;
  }
  // Point the shim's prototype at the original so instanceof checks continue to
  // work. Like the native constructor's, it is read-only.
  ObjectDefineProperty(Instance, 'prototype', {
    value: originalInstance.prototype,
    writable: false,
  });
  ObjectDefineProperty(Instance.prototype, 'constructor', {
    value: Instance,
    writable: true,
    configurable: true,
  });
  ObjectDefineProperty(wasm, 'Instance', {
    value: Instance,
    writable: true,
    configurable: true,
  });
}

export type WasmInstantiateShimExports = {
  install: typeof install;
};

module.exports = ObjectFreeze({
  install,
});
