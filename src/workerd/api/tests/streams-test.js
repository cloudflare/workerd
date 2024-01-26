import {
  strictEqual,
  ok,
  throws
} from 'node:assert';

const enc = new TextEncoder();

export const rs = {
  async test(ctrl, env) {
    const resp = await env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: new ReadableStream({
        expectedLength: 10,
        start(c) {
          c.enqueue(enc.encode('hellohello'));
          c.close();
        }
      })
    });
    for await (const _ of resp.body) {}
  }
};

export const ts = {
  async test(ctrl, env) {
    const { readable, writable } = new TransformStream({
      expectedLength: 10,
    });
    const writer = writable.getWriter();
    writer.write(enc.encode('hellohello'));
    writer.close();
    const resp = await env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: readable
    });
    for await (const _ of resp.body) {}
  }
};

export default {
  async fetch(request, env) {
    strictEqual(request.headers.get('content-length'), '10');
    return new Response(request.body);
  }
};

