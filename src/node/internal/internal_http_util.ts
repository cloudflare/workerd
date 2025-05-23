// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

export function once<RT>(
  this: unknown,
  callback: (...allArgs: unknown[]) => RT,
  { preserveReturnValue = false } = {}
): (...all: unknown[]) => RT {
  let called = false;
  let returnValue: RT;
  return function (this: unknown, ...args: unknown[]): RT {
    if (called) return returnValue;
    called = true;
    const result = Reflect.apply(callback, this, args);
    returnValue = preserveReturnValue ? result : (undefined as RT);
    return result;
  };
}
