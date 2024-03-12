
import {
  format,
  formatWithOptions,
} from 'node-internal:internal_inspect';

let debugImpls : object = {};

function debuglogImpl(set: string) {
  if ((debugImpls as any)[set] === undefined) {
    (debugImpls as any)[set] = function debug(...args : any[]) {
      const msg = formatWithOptions({ }, ...args);
      console.log(format('%s: %s\n', set, msg));
    };
  }
  return (debugImpls as any)[set];
}

// In Node.js' implementation, debuglog availability is determined by the NODE_DEBUG
// environment variable. However, we don't have access to the environment variables
// in the same way. Instead, we'll just always enable debuglog on the requested sets.
export function debuglog(set : string, cb? : (debug : (...args : any[]) => void) => void) {
  function init() {
    set = set.toUpperCase();
  }
  let debug = (...args : any[]) => {
    init();
    debug = debuglogImpl(set);
    if (typeof cb === 'function') {
      cb(debug);
    }
    switch (args.length) {
      case 1: return debug(args[0]);
      case 2: return debug(args[0], args[1]);
      default: return debug(...args);
    }
  };
  const logger = (...args : any[]) => {
    switch (args.length) {
      case 1: return debug(args[0]);
      case 2: return debug(args[0], args[1]);
      default: return debug(...args);
    }
  };
  Object.defineProperty(logger, 'enabled', {
    get() { return true; },
    configurable: true,
    enumerable: true,
  });
  return logger;
}
