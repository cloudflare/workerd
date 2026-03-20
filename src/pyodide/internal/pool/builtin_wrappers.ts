import type { getRandomValues as getRandomValuesType } from 'pyodide-internal:topLevelEntropy/lib';
import type { default as UnsafeEvalType } from 'internal:unsafe-eval';
import { PythonWorkersInternalError } from 'pyodide-internal:util';
import { PyodideVersion } from 'pyodide-internal:const';

if (typeof FinalizationRegistry === 'undefined') {
  // @ts-expect-error cannot assign to globalThis
  globalThis.FinalizationRegistry = class FinalizationRegistry {
    register(): void {}
    unregister(): void {}
  };
}

// Pyodide uses `new URL(some_url, location)` to resolve the path in `loadPackage`. Setting
// `location = undefined` makes this throw an error if some_url is not an absolute url. Which is what
// we want here, it doesn't make sense to load a package from a relative URL.
export const location = undefined;

export function addEventListener(): void {}

// Mostly we use the `jsglobals` variable for everything except for:
// * Pyodide's scheduler.ts
// * Emscripten's implementation of syscalls etc
//
// These locations use the globals directly so they will get them from the pool isolate unless we
// modify globalThis to include setTimeout etc from the main isolate.
// We could just change scheduler.ts to use jsglobals but it won't fix Emscripten so we'll need to
// do this either way.
export function setSetTimeout(
  st: typeof setTimeout,
  ct: typeof clearTimeout,
  si: typeof setInterval,
  ci: typeof clearInterval
): void {
  globalThis.setTimeout = st;
  globalThis.clearTimeout = ct;
  globalThis.setInterval = si;
  globalThis.clearInterval = ci;
}

export function reportUndefinedSymbolsPatched(Module: Module): void {
  if (Module.API.version === PyodideVersion.V0_26_0a2) {
    return;
  }
  Module.reportUndefinedSymbols();
}

function dynlibLookup026Helper(
  Module: Module,
  path: string
): string | undefined {
  try {
    Module.FS.lookupPath(path);
  } catch (e) {
    return undefined;
  }
  return path;
}

function dynlibLookup026(Module: Module, libName: string): string {
  // This function is for 0.26.0a2 only. In newer versions, we set LD_LIBRARY_PATH instead.
  if (Module.API.version !== PyodideVersion.V0_26_0a2) {
    throw new PythonWorkersInternalError('Should not happen');
  }
  // Most libraries are loaded from /usr/lib. For scipy and similar libraries that depend on
  // Pyodide's dynamic library deps, we may need extra "system libraries". These we'll put in
  // python_modules/lib. So try loading system libraries from there too.
  const result =
    dynlibLookup026Helper(Module, '/usr/lib/' + libName) ??
    dynlibLookup026Helper(
      Module,
      '/session/metadata/python_modules/lib/' + libName
    );
  if (!result) {
    console.error('Failed to read ', libName);
    throw new PythonWorkersInternalError('Should not happen');
  }
  return result;
}

export function patchedLoadLibData(
  Module: Module,
  path: string,
  rpath: any
): WebAssembly.Module {
  if (!path.startsWith('/')) {
    if (Module.API.version === PyodideVersion.V0_26_0a2) {
      path = dynlibLookup026(Module, path);
    } else {
      path = Module.findLibraryFS(path, rpath);
    }
  }
  return Module.compileModuleFromReadOnlyFS(Module, path);
}

export function patchedApplyFunc(
  API: API,
  func: (...params: any[]) => any,
  this_: object,
  args: any[]
): any {
  return API.config.jsglobals.Function.prototype.apply.apply(func, [
    this_,
    args,
  ]);
}

let getRandomValuesInner: typeof getRandomValuesType;
export function setGetRandomValues(func: typeof getRandomValuesType): void {
  getRandomValuesInner = func;
}

export function getRandomValues(Module: Module, arr: Uint8Array): Uint8Array {
  return getRandomValuesInner(Module, arr);
}

// We can't import UnsafeEval directly here because it isn't available when setting up Python pool.
// Thus, we inject it from outside via this function.
let UnsafeEval: typeof UnsafeEvalType;
export function setUnsafeEval(mod: typeof UnsafeEvalType): void {
  UnsafeEval = mod;
}

let lastTime: number;
let lastDelta = 0;
/**
 * Wrapper for Date.now that always advances by at least a millisecond. So that
 * directories change their modification time when updated so that Python
 * doesn't use stale directory contents in its import system.
 */
export function monotonicDateNow(): number {
  const now = Date.now();
  if (now === lastTime) {
    lastDelta++;
  } else {
    lastTime = now;
    lastDelta = 0;
  }
  return now + lastDelta;
}

/**
 * First check that the callee is what we expect, then use `UnsafeEval` to
 * construct a `WasmModule`.
 *
 * What we expect of the callee is that:
 * 1. it's in pyodide.asm.js
 * 2. it's in one of the locations that are required for it to work. We can
 *    pretty easily make a whitelist of these.
 *
 * In particular, we specifically don't want to allow calls from places that
 * call arbitrary functions for the user like `JsvFunction_CallBound` or
 * `raw_call_js`; if a user somehow gets their hands on a reference to
 * `newWasmModule` and tries to call it from Python the call would come from one
 * of these places. Currently we only need to allow `convertJsFunctionToWasm`
 * but if we enable JSPI we'll need to whitelist a few more locations.
 *
 * Some remarks:
 * 1. I don't really think that this `builtin_wrappers.newWasmModule` function
 *    can leak from `pyodide.asm.js`, but the code for `pyodide.asm.js` is
 *    generated and so difficult to analyze. I think the correct thing to do
 *    from a security analysis perspective is to assume that unreviewed
 *    generated code leaks all permissions it receives.
 * 2. Assuming user code somehow gets direct access to
 *    `builtin_wrappers.newWasmModule` I don't think it can spoof a call that
 *    passes this check.
 * 3. In normal Python code, this will only be called a fixed number of times
 *    every time we load a .so file. If we ever get to the position where
 *    `checkCallee` is a performance bottleneck, that would be a great success.
 *    Using ctypes, one can arrange to call a lot more times by repeatedly
 *    allocating and discarding closures. But:
 *      - ctypes is quite slow even by Python's standards
 *      - Normally ctypes allocates all closures up front
 */
let finishedSetup = false;
export function finishSetup(): void {
  finishedSetup = true;
}

export function newWasmModule(buffer: Uint8Array): WebAssembly.Module {
  if (!UnsafeEval) {
    return new WebAssembly.Module(buffer);
  }
  if (finishedSetup) {
    checkCallee();
  }
  return UnsafeEval.newWasmModule(buffer);
}

export function wasmInstantiate(
  mod: WebAssembly.Module | Uint8Array,
  imports: WebAssembly.Imports
): Promise<{ module: WebAssembly.Module; instance: WebAssembly.Instance }> {
  let module;
  if (mod instanceof WebAssembly.Module) {
    module = mod;
  } else {
    if (finishedSetup) {
      checkCallee();
    }
    module = UnsafeEval.newWasmModule(mod);
  }
  const instance = new WebAssembly.Instance(module, imports);
  return Promise.resolve({ module, instance });
}

/**
 * Check that the callee is `convertJsFunctionToWasm` by formatting a stack
 * trace and using `prepareStackTrace` to read out the callee. It should be
 * `convertJsFunctionToWasm` in `"pyodide-internal:generated/pyodide.asm"`,
 * if it's anything else we'll bail.
 */
function checkCallee(): void {
  const origPrepareStackTrace = Error.prepareStackTrace;
  let isOkay, funcName;
  try {
    Error.prepareStackTrace = prepareStackTrace;
    [isOkay, funcName] = new Error().stack as unknown as ReturnType<
      typeof prepareStackTrace
    >;
  } finally {
    Error.prepareStackTrace = origPrepareStackTrace;
  }
  if (!isOkay) {
    console.warn('Invalid call to `WebAssembly.Module`', funcName);
    throw new PythonWorkersInternalError(
      'Invalid call to `WebAssembly.Module`'
    );
  }
}

/**
 * Helper function for checkCallee, returns `true` if the callee is `convertJsFunctionToWasm`,
 * `generate`, or `getPyEMCountArgsPtr` in `pyodide.asm.js`, `false` if not. This will set the
 * `stack` field in the error so we can read back the result there.
 */
function prepareStackTrace(
  _error: Error,
  stack: StackItem[]
): [boolean, string] {
  // In case a logic error is ever introduced in this function, defend against
  // reentrant calls by setting `prepareStackTrace` to `undefined`.
  Error.prepareStackTrace = undefined;
  // Counting up, the bottom of the stack is `checkCallee`, then
  // `newWasmModule`, and the third entry should be our callee.
  if (stack.length < 3) {
    return [false, ''];
  }
  try {
    const funcName = stack[2].getFunctionName();
    const fileName = stack[2].getFileName();
    if (fileName !== 'pyodide-internal:generated/emscriptenSetup') {
      return [false, funcName];
    }
    return [
      ['convertJsFunctionToWasm', 'generate', 'getPyEMCountArgsPtr'].includes(
        funcName
      ),
      funcName,
    ];
  } catch (e) {
    console.warn(e);
    return [false, ''];
  }
}

/**
 * This is a fix for a problem with package snapshots in 0.26.0a2. 0.26.0a2 tests if
 * wasm-type-reflection is supported by the runtime and if so uses it to avoid function pointer
 * casting instead of a JS trampoline. We cannot stack switch through the JS trampoline so we need
 * to make sure that when stack switching is available, we don't use JS trampolines. When 0.26.0a2
 * was released, wasm-stack-switching implied wasm-type-reflection.
 *
 * Unfortunately there is a bug (fixed in 0.26.0a3...) that made the assumption that if the runtime
 * that we use to make a snapshot supports wasm-type-reflection, the runtime we use it in will too.
 *
 * Later, JSPI was rewritten not to depend on wasm-type-reflection, but the implication was left in
 * the v8 codebase until a few weeks ago. So our snapshots expect to be able to use an implementation
 * of `patched_PyEM_CountFuncParams` based on wasm-type-reflection, but it's not on anymore.
 *
 * Luckily, in the meantime there is a way to count the arguments of a webassembly function using
 * wasm-gc. It's not exactly pretty though.
 * This is copied from https://github.com/python/cpython/blob/main/Python/emscripten_trampoline.c
 */
// prettier-ignore
function getCountFuncParams(Module: Module): (funcPtr: number) => number {
  const code = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, // \0asm magic number
    0x01, 0x00, 0x00, 0x00, // version 1
    0x01, 0x1b, // Type section, body is 0x1b bytes
        0x05, // 6 entries
        0x60, 0x00, 0x01, 0x7f,                         // (type $type0 (func (param) (result i32)))
        0x60, 0x01, 0x7f, 0x01, 0x7f,                   // (type $type1 (func (param i32) (result i32)))
        0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,             // (type $type2 (func (param i32 i32) (result i32)))
        0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,       // (type $type3 (func (param i32 i32 i32) (result i32)))
        0x60, 0x01, 0x7f, 0x00,                         // (type $blocktype (func (param i32) (result)))
    0x02, 0x09, // Import section, 0x9 byte body
        0x01, // 1 import (table $funcs (import "e" "t") 0 funcref)
        0x01, 0x65, // "e"
        0x01, 0x74, // "t"
        0x01,       // importing a table
        0x70,       // of entry type funcref
        0x00, 0x00, // table limits: no max, min of 0
    0x03, 0x02,   // Function section
        0x01, 0x01, // We're going to define one function of type 1 (func (param i32) (result i32))
    0x07, 0x05, // export section
        0x01, // 1 export
        0x01, 0x66, // called "f"
        0x00, // a function
        0x00, // at index 0

    0x0a, 0x44,  // Code section,
        0x01, 0x42, // one entry of length 50
        0x01, 0x01, 0x70, // one local of type funcref
        // Body of the function
        0x20, 0x00,       // local.get $fptr
        0x25, 0x00,       // table.get $funcs
        0x22, 0x01,       // local.tee $fref
        0xfb, 0x14, 0x03, // ref.test $type3
        0x02, 0x04,       // block $b (type $blocktype)
            0x45,         //   i32.eqz
            0x0d, 0x00,   //   br_if $b
            0x41, 0x03,   //   i32.const 3
            0x0f,         //   return
        0x0b,             // end block

        0x20, 0x01,       // local.get $fref
        0xfb, 0x14, 0x02, // ref.test $type2
        0x02, 0x04,       // block $b (type $blocktype)
            0x45,         //   i32.eqz
            0x0d, 0x00,   //   br_if $b
            0x41, 0x02,   //   i32.const 2
            0x0f,         //   return
        0x0b,             // end block

        0x20, 0x01,       // local.get $fref
        0xfb, 0x14, 0x01, // ref.test $type1
        0x02, 0x04,       // block $b (type $blocktype)
            0x45,         //   i32.eqz
            0x0d, 0x00,   //   br_if $b
            0x41, 0x01,   //   i32.const 1
            0x0f,         //   return
        0x0b,             // end block

        0x20, 0x01,       // local.get $fref
        0xfb, 0x14, 0x00, // ref.test $type0
        0x02, 0x04,       // block $b (type $blocktype)
            0x45,         //   i32.eqz
            0x0d, 0x00,   //   br_if $b
            0x41, 0x00,   //   i32.const 0
            0x0f,         //   return
        0x0b,             // end block

        0x41, 0x7f,       // i32.const -1
        0x0b // end function
  ]);
  const mod = UnsafeEval.newWasmModule(code);
  const inst = new WebAssembly.Instance(mod, { e: { t: Module.wasmTable } });
  return inst.exports.f as ReturnType<typeof getCountFuncParams>;
}

let countFuncParams: (funcPtr: number) => number;

export function patched_PyEM_CountFuncParams(Module: Module, funcPtr: any) {
  countFuncParams ??= getCountFuncParams(Module);
  return countFuncParams(funcPtr);
}
