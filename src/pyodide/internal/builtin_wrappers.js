import { default as UnsafeEval } from "internal:unsafe-eval";
import { default as DiskCache } from "pyodide-internal:disk_cache";

let lastTime;
let lastDelta = 0;
/**
 * Wrapper for Date.now that always advances by at least a millisecond. So that
 * directories change their modification time when updated so that Python
 * doesn't use stale directory contents in its import system.
 */
export function monotonicDateNow() {
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
 * We initialize Python at top level, but it tries to initialize the random seed with
 * crypto.getRandomValues which will fail at top level. So we don't produce any entropy the first
 * time around and we reseed the rng in the first request context before executing user code.
 */
export function getRandomValues(arr) {
  try {
    return crypto.getRandomValues(arr);
  } catch (e) {
    if (e.message.includes("Disallowed operation called within global scope")) {
      // random.seed() can't work at startup. We'll seed again under the request scope.
      return arr;
    }
    throw e;
  }
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
 * `raw_call_js`; if a user somehow gets there hands on a reference to
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
 *    `checkCallee` is a performance bottleneck, that would be a great successs.
 *    Using ctypes, one can arrange to call a lot more times by repeatedly
 *    allocating and discarding closures. But:
 *      - ctypes is quite slow even by Python's standards
 *      - Normally ctypes allocates all closures up front
 */
export function newWasmModule(buffer) {
  checkCallee();
  return UnsafeEval.newWasmModule(buffer);
}

/**
 * Check that the callee is `convertJsFunctionToWasm` by formatting a stack
 * trace and using `prepareStackTrace` to read out the callee. It should be
 * `convertJsFunctionToWasm` in `"pyodide-internal:generated/pyodide.asm"`,
 * if it's anything else we'll bail.
 */
function checkCallee() {
  const origPrepareStackTrace = Error.prepareStackTrace;
  let isOkay;
  try {
    Error.prepareStackTrace = prepareStackTrace;
    isOkay = new Error().stack;
  } finally {
    Error.prepareStackTrace = origPrepareStackTrace;
  }
  if (!isOkay) {
    console.warn("Invalid call to `WebAssembly.Module`");
    throw new Error();
  }
}

/**
 * Helper function for checkCallee, returns `true` if the callee is
 * `convertJsFunctionToWasm` or `loadModule` in `pyodide.asm.js`, `false` if not. This will set
 * the `stack` field in the error so we can read back the result there.
 */
function prepareStackTrace(_error, stack) {
  // In case a logic error is ever introduced in this function, defend against
  // reentrant calls by setting `prepareStackTrace` to `undefined`.
  Error.prepareStackTrace = undefined;
  // Counting up, the bottom of the stack is `checkCallee`, then
  // `newWasmModule`, and the third entry should be our callee.
  if (stack.length < 3) {
    return false;
  }
  try {
    const funcName = stack[2].getFunctionName();
    const fileName = stack[2].getFileName();
    if (fileName !== "pyodide-internal:generated/pyodide.asm") {
      return false;
    }
    return ["loadModule", "convertJsFunctionToWasm"].includes(funcName);
  } catch (e) {
    console.warn(e);
    return false;
  }
}

export async function wasmInstantiate(module, imports) {
  if (!(module instanceof WebAssembly.Module)) {
    checkCallee();
    module = UnsafeEval.newWasmModule(module);
  }
  const instance = new WebAssembly.Instance(module, imports);
  return { module, instance };
}

export function patchFetch(origin) {
  // Patch fetch to first go through disk cache, but only when url points to origin
  const origFetch = globalThis.fetch;
  globalThis.fetch = async function (url, options) {
    if(url.origin !== origin) {
      return origFetch(url, options);
    }

    const fileName = url.pathname.substring(url.pathname.lastIndexOf("/") + 1);
    const cached = DiskCache.get(fileName);
    if (cached) {
      console.log("Serving from disk cache: " + fileName);
      return new Response(cached);
    }

    console.log("Loading from web: " + fileName);

    // we didn't find it in the disk cache, continue with original fetch
    const response = await origFetch(url, options);
    const arrayBuffer = await response.arrayBuffer();
    DiskCache.put(fileName, arrayBuffer);
    return new Response(arrayBuffer);
  };
}
