// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// WebAssembly compiles while handling a request, with and without the webassembly_streaming and
// webassembly_dynamic_compilation compatibility flags.

import assert from 'node:assert';

// An empty module exporting nothing: the magic number and version 1.
const bytes = () => new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);

const wasmResponse = (body) =>
  new Response(body, { headers: { 'Content-Type': 'application/wasm' } });

// A test that runs only in the service with the flags `enabled` or only in the one without them.
const when = (enabled, test) => ({
  async test(ctrl, env) {
    if (env.enabled === enabled) await test(env);
  },
});
const withFlags = (test) => when(true, test);

// A test that starts work at `path` in a request that ends without waiting for it, then compiles.
const pendingWhenItsRequestEnds = (path) =>
  withFlags(async (env) => {
    const response = await env.self.fetch(`http://example.com${path}`);
    assert.strictEqual(await response.text(), 'started');
    assert.ok(
      (await WebAssembly.compile(bytes())) instanceof WebAssembly.Module
    );
  });

export default {
  // Starts work that the request doesn't wait for, so that it is pending when the request ends.
  async fetch(request) {
    const path = new URL(request.url).pathname;
    if (path === '/compile') {
      WebAssembly.compile(bytes());
    } else if (path === '/stream') {
      // A body that never ends.
      const body = new ReadableStream({
        start(controller) {
          controller.enqueue(bytes());
        },
      });
      WebAssembly.compileStreaming(wasmResponse(body));
    }
    return new Response('started');
  },
};

export const compileCopiesTheBytes = withFlags(async () => {
  const source = bytes();
  const promise = WebAssembly.compile(source);
  // Compiling these would fail.
  source[0] = 1;
  assert.ok((await promise) instanceof WebAssembly.Module);
});

export const instantiate = withFlags(async () => {
  const { module, instance } = await WebAssembly.instantiate(bytes());
  assert.ok(instance instanceof WebAssembly.Instance);
  // Instantiating a compiled Module compiles nothing.
  assert.ok(
    (await WebAssembly.instantiate(module)) instanceof WebAssembly.Instance
  );
});

export const moduleConstructor = withFlags(() => {
  assert.ok(new WebAssembly.Module(bytes()) instanceof WebAssembly.Module);
});

export const compileStreaming = withFlags(async () => {
  const body = new ReadableStream({
    start(controller) {
      controller.enqueue(new Uint8Array([0, 97, 115, 109]));
      controller.enqueue(new Uint8Array([1, 0, 0, 0]));
      controller.close();
    },
  });
  const module = await WebAssembly.compileStreaming(wasmResponse(body));
  assert.ok(module instanceof WebAssembly.Module);
  const { instance } = await WebAssembly.instantiateStreaming(
    Promise.resolve(wasmResponse(bytes()))
  );
  assert.ok(instance instanceof WebAssembly.Instance);
});

export const evalStaysDisallowed = withFlags(() => {
  assert.throws(() => eval('1'), { name: 'EvalError' });
});

export const concurrentCompilesAllSettle = withFlags(async () => {
  const modules = await Promise.all(
    Array.from({ length: 50 }, () => WebAssembly.compile(bytes()))
  );
  assert.ok(modules.every((module) => module instanceof WebAssembly.Module));
});

export const compileRejectsADataView = withFlags(async () => {
  await assert.rejects(WebAssembly.compile(new DataView(bytes().buffer)), {
    name: 'TypeError',
  });
});

export const compilePendingWhenItsRequestEnds =
  pendingWhenItsRequestEnds('/compile');

export const streamPendingWhenItsRequestEnds =
  pendingWhenItsRequestEnds('/stream');

export const disabled = when(false, async () => {
  assert.strictEqual(WebAssembly.compileStreaming, undefined);
  assert.strictEqual(WebAssembly.instantiateStreaming, undefined);
  await assert.rejects(WebAssembly.compile(bytes()), { name: 'CompileError' });
  assert.throws(() => new WebAssembly.Module(bytes()), {
    name: 'CompileError',
  });
});
