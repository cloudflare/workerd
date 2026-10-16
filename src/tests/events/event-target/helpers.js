// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Helpers for the modules that observe reported listener exceptions or
// need the C++ classes.

// Runs fn with a global 'error' listener, and returns what that listener
// saw: the exception of every 'error' event, in order. The listener calls
// preventDefault(), which keeps the exceptions off the console. (The
// runtime's ErrorEvent is not cancelable, as HTML's is, but the global logs
// an exception only when its dispatch reports the event uncanceled, which
// in C++ ignores cancelability: ledger #2.)
export function collectReportedErrors(fn) {
  const reported = [];
  const listener = (event) => {
    reported.push(event.error);
    event.preventDefault();
  };
  globalThis.addEventListener('error', listener);
  try {
    fn();
  } finally {
    globalThis.removeEventListener('error', listener);
  }
  return reported;
}

// The C++ Event and EventTarget, which the C++ targets (the global,
// AbortSignal) and the runtime's own events (MessageEvent and the rest)
// are. They are the globals in the C++ cells; in the TS cell the globals are
// the TS classes (ledger #20).
export const NativeEvent = Object.getPrototypeOf(
  MessageEvent.prototype
).constructor;
export const NativeEventTarget = Object.getPrototypeOf(
  AbortSignal.prototype
).constructor;
