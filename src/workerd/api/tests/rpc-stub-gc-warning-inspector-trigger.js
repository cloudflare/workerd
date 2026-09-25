// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

class Counter extends RpcTarget {
  increment() {
    return 1;
  }
}

export class MyService extends WorkerEntrypoint {
  getCounter() {
    return new Counter();
  }
}

const chunk = 'x'.repeat(4096);

function buildConsString() {
  let result = chunk;
  for (let i = 0; i < 16; i++) result += chunk;
  return result;
}

// charCodeAt() flattens the ConsString. Once V8 optimizes this function, an allocation failure
// can start a minor GC from a runtime call whose optimized frame has no deoptimization metadata.
// If a leaked-stub finalizer runs in that GC and tries to capture a JS stack trace for the
// inspector, V8 aborts the process.
function scan(str) {
  let hash = 0;
  for (let i = 0; i < str.length; i += 4093) {
    hash = (hash * 31 + str.charCodeAt(i)) | 0;
  }
  return hash;
}

export default {
  async fetch(request, env) {
    const dispose = new URL(request.url).searchParams.has('dispose');

    if (dispose) {
      let stub = await env.MyService.getCounter();
      stub[Symbol.dispose]();
      stub = null;
      gc();
      gc();
      return new Response('disposed');
    }

    let hash = 0;
    for (let i = 0; i < 30000; i++) hash = (hash + scan(buildConsString())) | 0;

    let stubs = [];
    for (let i = 0; i < 32; i++) {
      const stub = await env.MyService.getCounter();
      await stub.increment();
      stubs.push(stub);
    }
    stubs = null;

    // Allocation-driven collections in scan() exercise the original crash. Explicit collections
    // ensure that the warning is emitted even when heap sizing does not trigger one in this loop.
    for (let i = 0; i < 20000; i++) hash = (hash + scan(buildConsString())) | 0;
    gc();
    gc();

    return new Response(`leaked: ${hash}`);
  },
};
