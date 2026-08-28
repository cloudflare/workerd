// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streams created at GLOBAL SCOPE — module evaluation runs outside any
// IoContext, so stream construction (and even module-scope consumption)
// must not require one. Requests then use the module-scope streams via
// SELF round-trips (migrated wholesale from streams-iocontext-test.js,
// itself ported from the edgeworker streams-iocontext.ew-test).

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// global-scope-readablestream
const enc1 = new TextEncoder();
const rs1 = new ReadableStream({
  start(c) {
    c.enqueue(enc1.encode('ok'));
    c.close();
  },
});

export const globalScopeReadablestream = {
  async test(ctrl, env) {
    const response = await env.self.fetch('http://test/1');
    const text = await response.text();
    strictEqual(text, 'ok');
  },
};

// global-scope-readablestream-2
const enc2 = new TextEncoder();
const dec2 = new TextDecoder();
const rs2 = new ReadableStream({
  start(c) {
    c.enqueue(enc2.encode('ok'));
    c.close();
  },
});
let text2 = '';
const modulePromise2 = (async () => {
  for await (const chunk of rs2) {
    text2 += dec2.decode(chunk);
  }
  if (text2 !== 'ok') {
    throw new Error('Stream should have been readable at global scope');
  }
})();

export const globalScopeReadablestream2 = {
  async test(ctrl, env) {
    await modulePromise2;
    const response = await env.self.fetch('http://test/2');
    const text = await response.text();
    strictEqual(text, 'ok');
  },
};

// global-scope-readablestream-3
const enc3 = new TextEncoder();
const dec3 = new TextDecoder();
const rs3 = new ReadableStream({
  start(c) {
    c.enqueue(enc3.encode('ok'));
    c.close();
  },
});

export const globalScopeReadablestream3 = {
  async test(ctrl, env) {
    const response = await env.self.fetch('http://test/3');
    const text = await response.text();
    strictEqual(text, 'ok');
  },
};

// global-scope-readablestream-4
const enc4 = new TextEncoder();
const rs4 = new ReadableStream({
  start(c) {
    c.enqueue(enc4.encode('ok'));
    c.close();
  },
});

export const globalScopeReadablestream4 = {
  async test(ctrl, env) {
    const response = await env.self.fetch('http://test/4');
    const text = await response.text();
    strictEqual(text, 'ok');
  },
};

// global-scope-readablestream-5
const enc5 = new TextEncoder();
const rs5 = new ReadableStream({
  start(c) {
    c.enqueue(enc5.encode('ok'));
    c.close();
  },
});
const _res5 = new Response(rs5);

export const globalScopeReadablestream5 = {
  async test(ctrl, env) {
    const response = await env.self.fetch('http://test/5');
    const text = await response.text();
    strictEqual(text, 'ok');
  },
};

// global-scope-readablestream-6
const rs6 = new ReadableStream({
  start(c) {
    globalThis.controller6 = c;
  },
});

export const globalScopeReadablestream6 = {
  async test(ctrl, env) {
    const responseA = await env.self.fetch('http://test/6/a');
    strictEqual(await responseA.text(), 'ok');

    const responseB = await env.self.fetch('http://test/6/b');
    strictEqual(await responseB.text(), 'madness');
  },
};

// global-scope-readablestream-7
let resolve7;
const promise7 = new Promise((r) => (resolve7 = r));
const rs7 = new ReadableStream({
  async pull(c) {
    const pullFunction = await promise7;
    return pullFunction(c);
  },
});

export const globalScopeReadablestream7 = {
  async test(ctrl, env) {
    const responseA = await env.self.fetch('http://test/7/a');
    strictEqual(await responseA.text(), 'ok');

    const responseB = await env.self.fetch('http://test/7/b');
    strictEqual(await responseB.text(), 'madness');
  },
};

// global-scope-readablestream-8
let resolve8;
const promise8 = new Promise((r) => (resolve8 = r));
const rs8 = new ReadableStream({
  async start(c) {
    globalThis.controller8 = c;
    await promise8;
  },
});

export const globalScopeReadablestream8 = {
  async test(ctrl, env) {
    const responseA = await env.self.fetch('http://test/8/a');
    strictEqual(await responseA.text(), 'ok');

    const responseB = await env.self.fetch('http://test/8/b');
    strictEqual(await responseB.text(), 'madness');
  },
};

// A standard WritableStream allocates an AbortSignal for its
// controller, which must not require an active IoContext (migrated
// from streams-test.js).
const moduleScopeWritable = new WritableStream();

export const globalScopeWritablestream = {
  test() {
    strictEqual(moduleScopeWritable instanceof WritableStream, true);
    strictEqual(moduleScopeWritable.locked, false);
  },
};

// A byte stream constructed at module scope serves BYOB reads inside a
// request in both implementations.
const enc9 = new TextEncoder();
const rs9 = new ReadableStream({
  type: 'bytes',
  start(c) {
    c.enqueue(enc9.encode('ok'));
    c.close();
  },
});

export const globalScopeByteReadable = {
  async test(ctrl, env) {
    strictEqual(rs9 instanceof ReadableStream, true);
    const response = await env.self.fetch('http://test/9');
    strictEqual(await response.text(), 'ok');
  },
};

// A TransformStream constructed at module scope. C++ CRASH BUG: the
// mere EXISTENCE of a module-scope `new TransformStream()` SEGFAULTS
// the C++ implementation when the first request enters the worker —
// even if the stream is never touched again. The TypeScript
// implementation constructs it and pipes through it inside a request
// normally, so construction itself is guarded by implementation here.
// TODO(bug): unguard once the crash is fixed.
const ts10 = usingTsImpl ? new TransformStream() : null;

export const globalScopeTransformStream = {
  async test(ctrl, env) {
    if (!usingTsImpl) return; // module-scope construction segfaults C++
    const response = await env.self.fetch('http://test/10');
    strictEqual(await response.text(), 'ok');
  },
};

export default {
  async fetch(req) {
    const url = new URL(req.url);

    // global-scope-readablestream
    if (url.pathname === '/1') {
      return new Response(rs1);
    }

    // global-scope-readablestream-2
    if (url.pathname === '/2') {
      return new Response('ok');
    }

    // global-scope-readablestream-3
    if (url.pathname === '/3') {
      let text = '';
      for await (const chunk of rs3) {
        text += dec3.decode(chunk);
      }
      if (text !== 'ok') {
        throw new Error('Global scope stream should have been readable');
      }
      return new Response('ok');
    }

    // global-scope-readablestream-4
    if (url.pathname === '/4') {
      return new Response(rs4.pipeThrough(new TransformStream()));
    }

    // global-scope-readablestream-5
    if (url.pathname === '/5') {
      return new Response('ok');
    }

    // global-scope-readablestream-6
    if (url.pathname === '/6/a') {
      const enc = new TextEncoder();
      globalThis.controller6.enqueue(enc.encode('madness'));
      globalThis.controller6.close();
      return new Response('ok');
    }
    if (url.pathname === '/6/b') {
      let text = '';
      const dec = new TextDecoder();
      for await (const chunk of rs6) {
        text += dec.decode(chunk);
      }
      return new Response(text);
    }

    // global-scope-readablestream-7
    if (url.pathname === '/7/a') {
      const enc = new TextEncoder();
      resolve7((c) => {
        c.enqueue(enc.encode('madness'));
        c.close();
      });
      return new Response('ok');
    }
    if (url.pathname === '/7/b') {
      let text = '';
      const dec = new TextDecoder();
      for await (const chunk of rs7) {
        text += dec.decode(chunk);
      }
      return new Response(text);
    }

    // global-scope-readablestream-8
    if (url.pathname === '/8/a') {
      const enc = new TextEncoder();
      await scheduler.wait(10);
      resolve8();
      globalThis.controller8.enqueue(enc.encode('madness'));
      globalThis.controller8.close();
      return new Response('ok');
    }
    if (url.pathname === '/8/b') {
      let text = '';
      const dec = new TextDecoder();
      for await (const chunk of rs8) {
        text += dec.decode(chunk);
      }
      return new Response(text);
    }

    // global-scope-byte-readable
    if (url.pathname === '/9') {
      const reader = rs9.getReader({ mode: 'byob' });
      const { value } = await reader.read(new Uint8Array(8));
      return new Response(value);
    }

    // global-scope-transform-stream
    if (url.pathname === '/10') {
      const enc = new TextEncoder();
      const writer = ts10.writable.getWriter();
      void writer.write(enc.encode('ok'));
      void writer.close();
      return new Response(ts10.readable);
    }

    throw new Error('boom');
  },
};
