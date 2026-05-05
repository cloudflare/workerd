// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Callback used to report PythonWorkersInternalError construction to C++ metrics (via the
// WorkerFatalReporter module). Registered by `python.ts` at module init in the main workerd
// context.
//
// We can't `import` `pyodide-internal:fatal-reporter` (a C++ extension module) directly here
// because `util.ts` is also esbuild-bundled into the pool context (`pool/emscriptenSetup.ts`
// runs in a vanilla V8 isolate with no C++ extension module support). A static import would fail
// esbuild's resolver; a dynamic import fails at runtime because workerd's dynamic module
// resolver doesn't surface INTERNAL C++-backed modules. The callback indirection keeps
// `util.ts` free of any reference to the C++ module while still letting the main context wire
// in the real reporter.
//
// TODO: If we ever remove the Python pool, `util.ts` will no longer be pool-bundled and we can
// drop this callback in favor of a direct static import of FatalReporter.
let _reportInternalError: (() => void) | null = null;

export function setInternalErrorReporter(cb: () => void): void {
  _reportInternalError = cb;
}

/**
 * This is an exception we should be throwing whenever there is something unexpected in our runtime
 * that is **not** a result of the user doing something wrong, i.e. it's an internal error that is
 * a result of a bug in our runtime.
 */
export class PythonWorkersInternalError extends Error {
  constructor(message?: string) {
    super(message);
    try {
      _reportInternalError?.();
    } catch (_) {}
  }

  override get name(): string {
    return this.constructor.name;
  }
}

/**
 * This is an exception we throw whenever there is an issue with the user's code, i.e. it's a result
 * of the user doing something wrong.
 */
export class PythonUserError extends Error {
  override get name(): string {
    return this.constructor.name;
  }
}

// Split the stack into lines and print them individually.
// We do this because edgeworker's test runner will put a multiline log all on one line. This is
// very hard to read.
export function reportError(e: Error): never {
  e.stack?.split('\n').forEach((s: string) => {
    console.warn(s);
  });
  throw e;
}

/**
 *  Simple as possible runPython function which works with no foreign function
 *  interface. We need to use this rather than the normal easier to use
 *  interface because the normal interface doesn't work until after
 *  `API.finalizeBootstrap`, but `API.finalizeBootstrap` makes changes inside
 *  and outside the linear memory which have to stay in sync. It's hard to keep
 *  track of the invariants that `finalizeBootstrap` introduces between JS land
 *  and the linear memory so we do this.
 *
 *  We wrap API.rawRun which does the following steps:
 *  1. use textEncoder.encode to convert `code` into UTF8 bytes
 *  2. malloc space for `code` in the wasm linear memory and copy the encoded
 *      `code` to this pointer
 *  3. redirect standard error to a temporary buffer
 *  4. call `PyRun_SimpleString`, which either works and returns 0 or formats a
 *      traceback to stderr and returns -1
 *      https://docs.python.org/3/c-api/veryhigh.html?highlight=simplestring#c.PyRun_SimpleString
 *  5. frees the `code` pointer
 *  6. Returns the return value from `PyRun_SimpleString` and whatever
 *      information went to stderr.
 *
 *  PyRun_SimpleString executes the code at top level in the `__main__` module,
 *  so all variables defined get leaked into the global namespace unless we
 *  clean them up explicitly.
 */
export function simpleRunPython(
  emscriptenModule: Module,
  code: string
): string {
  const [status, cause] = emscriptenModule.API.rawRun(code);
  // status 0: Ok
  // status -1: Error
  if (status === -1) {
    // PyRun_SimpleString will have written a Python traceback to stderr.
    console.warn('Command failed:', code);
    console.warn(cause);
    throw new PythonWorkersInternalError(
      'Failed to run Python code:\n' + code + '\n\nError:\n' + cause
    );
  }
  return cause;
}

export function invalidateCaches(Module: Module): void {
  simpleRunPython(
    Module,
    `from importlib import invalidate_caches; invalidate_caches(); del invalidate_caches`
  );
}

export function unreachable(obj: never, msg?: string): never {
  if (msg === undefined) {
    msg = obj;
  }
  throw new PythonWorkersInternalError(`Unreachable: ${msg}`);
}

/**
 * Loads a Python source file (bundled as a Uint8Array) into a fresh anonymous
 * module and returns it. This is used for internal Python helpers that need to
 * be invoked from JS but should not pollute the global namespace.
 *
 * `moduleName` is the bare name of the module (e.g. "introspection"); typically
 * the source is the default export from `pyodide-internal:<moduleName>.py`.
 */
export function loadPythonMod(
  pyodide: Pyodide,
  moduleName: string,
  source: Uint8Array
): { __dict__: PyDict } {
  const mod = pyodide.runPython(
    `from types import ModuleType; ModuleType('${moduleName}')`
  ) as { __dict__: PyDict };
  const decoder = new TextDecoder();
  pyodide.runPython(decoder.decode(source), {
    globals: mod.__dict__,
    filename: `${moduleName}.py`,
  });
  return mod;
}
