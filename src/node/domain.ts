// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
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

// Of all the deprecated Node.js modules that we need to have available
// but don't want to actually implement, domain is the probably the
// most least likely to ever be implemented.

import { EventEmitter } from 'node-internal:events';
EventEmitter.usingDomains = false;

export class Domain extends EventEmitter {
  members: unknown[] | undefined = undefined;

  _errorHandler(_er: unknown): void {
    // Should never be called since we override everything that would call it.
    // But if it is, just throw the error that is passed in.
    throw _er;
  }

  enter(): void {
    // Instead of throwing, we just don't do anything here
  }

  exit(): void {
    // Insteading of throwing, we just don't do anything here.
  }

  add(_ee: unknown): void {
    // Insteading of throwing, we just don't do anything here.
  }

  remove(_ee: unknown): void {
    // Insteading of throwing, we just don't do anything here.
  }

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  run(fn: Function, ...args: unknown[]): unknown {
    // This is non-operational. We end up just calling the function directly.
    this.enter();
    const ret: unknown = Reflect.apply(fn, this, args);
    this.exit();
    return ret;
  }

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  intercept(cb: Function): Function {
    // This is non-operational. We end up just returning the callback directly.
    return cb;
  }

  // eslint-disable-next-line @typescript-eslint/no-unsafe-function-type
  bind(cb: Function): Function {
    // This is non-operational. We end up just returning the callback directly.
    return cb;
  }
}

export function createDomain(): Domain {
  return new Domain();
}
export const create = createDomain();

// The active domain is always the one that we're currently in.
export const active: Domain | null = null;

export default {
  Domain,
  active,
  createDomain,
  create,
};
