// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Makes an Error. The caller's name appears in the error's original stack,
// which shows where a stack came from.
export function makeError(where) {
  return new Error(`from ${where}`);
}

// Describes a received error: its message, where its stack came from
// ('remote' or 'local' for the original stack of an error made by
// remoteMakesError() or localMakesError(), 'new' for one captured on
// receipt), and whether the stack's first line gives its name and message.
export function describe(error) {
  const stack = error.stack;
  let origin = 'new';
  if (stack.includes('remoteMakesError')) origin = 'remote';
  if (stack.includes('localMakesError')) origin = 'local';
  return {
    message: error.message,
    origin,
    firstLineNamesError:
      stack.split('\n')[0] === `${error.name}: ${error.message}`,
  };
}
