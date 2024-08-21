import { ok, strictEqual } from 'node:assert';

const enc = new TextEncoder();

// This one returns false with no error thrown because we're not in an IoContext
ok(!navigator.sendBeacon('http://example.org', 'does not work'));

export const navigatorBeaconTest = {
  async test(ctrl, env, ctx) {
    ok(navigator.sendBeacon('http://example.org', 'beacon'));
    ok(
      navigator.sendBeacon(
        'http://example.org',
        new ReadableStream({
          start(c) {
            c.enqueue(enc.encode('beacon'));
            c.close();
          },
        })
      )
    );
    ok(navigator.sendBeacon('http://example.org', enc.encode('beacon')));
  },
};

export default {
  async fetch(req, env) {
    strictEqual(await req.text(), 'beacon');
    return new Response(null, { status: 204 });
  },
};
