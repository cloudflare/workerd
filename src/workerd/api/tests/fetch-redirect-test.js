// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';

export class Server extends WorkerEntrypoint {
  async fetch(req) {
    const endpoint = new URL(req.url).pathname;
    switch (endpoint) {
      case '/echo-authorization':
        return new Response(
          req.headers.get('authorization') ?? '<not provided>'
        );

      case '/redirect-same-origin':
        return Response.redirect('http://api.example.com/echo-authorization');

      case '/redirect-cross-origin':
        return Response.redirect(
          'http://totallynotahacker.com/echo-authorization'
        );

      case '/echo-body':
        return new Response(`${req.method} ${await req.text()}`);

      case '/redirect-307':
        return Response.redirect('http://api.example.com/echo-body', 307);

      case '/redirect-303':
        return Response.redirect('http://api.example.com/echo-body', 303);
    }
  }
}

// Fetches a 307 redirect with the given body, which fetch() has to send again to the target.
async function postThroughRedirect(env, body) {
  const res = await env.Server.fetch('http://api.example.com/redirect-307', {
    method: 'POST',
    body,
  });
  return await res.text();
}

export const redirectResendsInMemoryBodies = {
  async test(ctrl, env, ctx) {
    const text = 'h\u00e9llo \u{1F600} world';
    const bytes = new TextEncoder().encode(text);
    const cases = [
      [text, text],
      [bytes.buffer, text],
      [bytes, text],
      [new Uint8Array([0, ...bytes, 0]).subarray(1, bytes.length + 1), text],
      [new Blob([text]), text],
      [new URLSearchParams({ a: '1', b: 'two' }), 'a=1&b=two'],
    ];
    for (const [body, expected] of cases) {
      assert.strictEqual(
        await postThroughRedirect(env, body),
        `POST ${expected}`
      );
    }
  },
};

export const redirectResendsFormDataBody = {
  async test(ctrl, env, ctx) {
    const form = new FormData();
    form.append('field', 'value');
    const echoed = await postThroughRedirect(env, form);
    assert.match(echoed, /^POST --/);
    assert.match(echoed, /name="field"\r\n\r\nvalue\r\n/);
  },
};

export const redirectResendsLargeBody = {
  async test(ctrl, env, ctx) {
    const text = '\u00e9\u{1F600}x'.repeat(256 * 1024);
    assert.strictEqual(await postThroughRedirect(env, text), `POST ${text}`);
  },
};

export const redirectResendsBodyCopiedAtConstruction = {
  async test(ctrl, env, ctx) {
    // The body is a copy of the buffer taken when the Request is constructed, so neither changes
    // to the buffer nor detaching it afterwards affect what is sent, the first time or again
    // after the redirect.
    const buffer = new TextEncoder().encode('original').buffer;
    const req = new Request('http://api.example.com/redirect-307', {
      method: 'POST',
      body: buffer,
    });
    new Uint8Array(buffer).fill(0x78);
    structuredClone(buffer, { transfer: [buffer] });
    assert.strictEqual(buffer.byteLength, 0);
    const res = await env.Server.fetch(req);
    assert.strictEqual(await res.text(), 'POST original');
  },
};

export const redirectResendsBodyOfConstructedAndClonedRequests = {
  async test(ctrl, env, ctx) {
    const original = new Request('http://api.example.com/redirect-307', {
      method: 'POST',
      body: 'payload',
    });

    // A clone has its own body, and reading one doesn't consume the other.
    const clone = original.clone();
    assert.strictEqual(await clone.text(), 'payload');

    // `new Request(request)` takes over the body, along with its source.
    const taken = new Request(original);
    assert.strictEqual(original.bodyUsed, true);
    let res = await env.Server.fetch(taken);
    assert.strictEqual(await res.text(), 'POST payload');

    // A Request used as the init dictionary shares the body, along with its source.
    const init = new Request('http://example.com', {
      method: 'POST',
      body: 'shared',
    });
    res = await env.Server.fetch('http://api.example.com/redirect-307', init);
    assert.strictEqual(await res.text(), 'POST shared');
  },
};

export const redirectResendsBodyAfterItsStreamWasCreated = {
  async test(ctrl, env, ctx) {
    // Touching `body` creates the stream; fetch() sends that stream first and, after the
    // redirect, a new one created from the source.
    const req = new Request('http://api.example.com/redirect-307', {
      method: 'POST',
      body: 'payload',
    });
    assert.ok(req.body instanceof ReadableStream);

    // Cloning a body whose stream exists tees the stream; both bodies keep the source.
    const clone = req.clone();
    for (const r of [req, clone]) {
      const res = await env.Server.fetch(r);
      assert.strictEqual(await res.text(), 'POST payload');
    }
  },
};

export const redirectRejectsStreamBody = {
  async test(ctrl, env, ctx) {
    const body = new Blob(['payload']).stream();
    await assert.rejects(postThroughRedirect(env, body), {
      name: 'TypeError',
      message: /one-time-use body/,
    });
  },
};

export const redirect303DropsBody = {
  async test(ctrl, env, ctx) {
    const res = await env.Server.fetch('http://api.example.com/redirect-303', {
      method: 'POST',
      body: new Blob(['payload']).stream(),
    });
    assert.strictEqual(await res.text(), 'GET ');
  },
};

export const sameOriginRedirectPreservesAuthorization = {
  async test(ctrl, env, ctx) {
    const res = await env.Server.fetch(
      'http://api.example.com/redirect-same-origin',
      { headers: { Authorization: 's00persecret' } }
    );
    assert.strictEqual(await res.text(), 's00persecret');
  },
};

export const crossOriginRedirectStripsAuthorization = {
  async test(ctrl, env, ctx) {
    const res = await env.Server.fetch(
      'http://api.example.com/redirect-cross-origin',
      { headers: { Authorization: 's00persecret' } }
    );
    assert.strictEqual(await res.text(), '<not provided>');
  },
};
