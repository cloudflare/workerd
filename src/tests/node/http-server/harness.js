// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared machinery: a node http.Server per test, and the two ways a
// Request reaches it.
//
// - Through the service binding: `env.SERVICE.fetch(...)` re-enters this
//   worker, whose default handler (main.js) routes the Request to the
//   current server. Bodies are pumped across the binding by the runtime,
//   which is the production shape; a cancellation on one side reaches the
//   other only once the exchange completes.
// - Directly: `dispatch(new Request(...))` hands an in-isolate Request to
//   the server, so its body stream IS the test's stream and cancellation is
//   observable immediately.
//
// Tests run sequentially, so a single current port suffices.

import http from 'node:http';
import { handleAsNodeRequest } from 'cloudflare:node';

let currentPort = null;
let currentEnv;
let currentCtx;

export function route(request, env, ctx) {
  return handleAsNodeRequest(currentPort, request, env, ctx);
}

// Runs fn with a listening server built from handler, then closes it.
export async function withServer(handler, fn, options) {
  const server =
    options === undefined
      ? http.createServer(handler)
      : http.createServer(options, handler);
  await new Promise((resolve) => server.listen(0, resolve));
  currentPort = server.address().port;
  try {
    return await fn(server);
  } finally {
    server.close();
  }
}

export function remember(env, ctx) {
  currentEnv = env;
  currentCtx = ctx;
}

// The direct path (see above).
export function dispatch(request) {
  return handleAsNodeRequest(currentPort, request, currentEnv, currentCtx);
}

export function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// A ReadableStream the test feeds by hand.
export function manualStream() {
  let controller;
  const events = [];
  const stream = new ReadableStream({
    start(c) {
      controller = c;
    },
    cancel(reason) {
      events.push(reason);
    },
  });
  return { stream, controller, cancels: events };
}

// Runs fn while recording every uncaught exception and unhandled rejection
// the isolate reports (waiting a beat for late ones), and returns them;
// the caller decides which, if any, were expected.
export async function collectUncaught(fn) {
  const leaked = [];
  const onError = (event) => {
    leaked.push(event.error ?? event.message);
    event.preventDefault();
  };
  const onRejection = (event) => {
    leaked.push(event.reason);
    event.preventDefault();
  };
  globalThis.addEventListener('error', onError);
  globalThis.addEventListener('unhandledrejection', onRejection);
  try {
    await fn();
    await scheduler.wait(20);
  } finally {
    globalThis.removeEventListener('error', onError);
    globalThis.removeEventListener('unhandledrejection', onRejection);
  }
  return leaked;
}
