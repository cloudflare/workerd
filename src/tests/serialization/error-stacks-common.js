// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/* global CloneableTestError */

// Makes a native Error or a CloneableTestError. The caller's name appears in
// the error's original stack, which shows where a stack came from.
export function makeError(kind, where) {
  return kind === 'native'
    ? new Error(`native from ${where}`)
    : new CloneableTestError(`cloneable from ${where}`);
}

// Describes a received error: its class, its message, where its stack came
// from ('remote' or 'local' for the original stack of an error made by
// remoteMakesError() or localMakesError(), 'new' for one captured on
// receipt), whether the stack's first line gives its name and message, and
// whether the stack is enumerable.
export function describe(error) {
  const stack = error.stack;
  let origin = 'new';
  if (stack.includes('remoteMakesError')) origin = 'remote';
  if (stack.includes('localMakesError')) origin = 'local';
  return {
    cloneable: error instanceof CloneableTestError,
    message: error.message,
    origin,
    firstLineNamesError:
      stack.split('\n')[0] === `${error.name}: ${error.message}`,
    enumerable: Object.getOwnPropertyDescriptor(error, 'stack').enumerable,
  };
}
