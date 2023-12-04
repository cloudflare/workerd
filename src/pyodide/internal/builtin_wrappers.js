import {default as Eval} from "pyodide-internal:eval";

// Wrapper for WebAssembly that makes WebAssembly.Module work by temporarily
// turning on eval
const origWebAssembly = globalThis.WebAssembly;
export const WebAssembly = new Proxy(origWebAssembly, {get(target, val) {
  const result = Reflect.get(...arguments);
  if (val !== "Module") {
    return result;
  }
  return new Proxy(result, {
    construct() {
      try {
        Eval.enableEval();
        return Reflect.construct(...arguments);
      } finally {
        Eval.disableEval();
      }
    }
  });
}});


// Wrapper for Date.now that always advances by at least a millisecond.
let lastTime;
let lastDelta = 0;
const origDate = globalThis.Date;
export const Date = new Proxy(origDate, {get(target, val) {
  if (val === "now") {
    return function now() {
      const now = origDate.now();
      if (now === lastTime) {
        lastDelta ++;
      } else {
        lastTime = now;
        lastDelta = 0;
      }
      return now + lastDelta;
    }
  }
  return Reflect.get(...arguments);
}});
