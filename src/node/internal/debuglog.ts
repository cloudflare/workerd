// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { format, formatWithOptions } from 'node-internal:internal_inspect';

let debugImpls: object = {};

function debuglogImpl(set: string) {
  if ((debugImpls as any)[set] === undefined) {
    (debugImpls as any)[set] = function debug(...args: any[]) {
      const msg = formatWithOptions({}, ...args);
      console.log(format('%s: %s\n', set, msg));
    };
  }
  return (debugImpls as any)[set];
}

// In Node.js' implementation, debuglog availability is determined by the NODE_DEBUG
// environment variable. However, we don't have access to the environment variables
// in the same way. Instead, we'll just always enable debuglog on the requested sets.
export function debuglog(
  set: string,
  cb?: (debug: (...args: any[]) => void) => void
): any {
  function init() {
    set = set.toUpperCase();
  }
  let debug = (...args: any[]): void => {
    init();
    debug = debuglogImpl(set);
    if (typeof cb === 'function') {
      cb(debug);
    }
    switch (args.length) {
      case 1:
        return debug(args[0]);
      case 2:
        return debug(args[0], args[1]);
      default:
        return debug(...args);
    }
  };
  const logger = (...args: any[]) => {
    switch (args.length) {
      case 1:
        return debug(args[0]);
      case 2:
        return debug(args[0], args[1]);
      default:
        return debug(...args);
    }
  };
  Object.defineProperty(logger, 'enabled', {
    get() {
      return true;
    },
    configurable: true,
    enumerable: true,
  });
  return logger;
}
