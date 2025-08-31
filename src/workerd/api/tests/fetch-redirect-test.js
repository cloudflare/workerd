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
    }
  }
}

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
