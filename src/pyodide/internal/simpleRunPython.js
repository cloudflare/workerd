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
export function simpleRunPython(emscriptenModule, code) {
  const [status, err] = emscriptenModule.API.rawRun(code);
  // status 0: Ok
  // status -1: Error
  if (status) {
    // PyRun_SimpleString will have written a Python traceback to stderr.
    console.warn("Command failed:", code);
    console.warn("Error was:");
    for (const line of err.split("\n")) {
      console.warn(line);
    }
    throw new Error("Failed");
  }
}
