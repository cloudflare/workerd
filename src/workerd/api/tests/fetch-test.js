import { strictEqual, throws } from 'assert';

// Test depends on the setting of the upper_case_all_http_methods compatibility flag.
strictEqual(
  globalThis.Cloudflare.compatibilityFlags['upper_case_all_http_methods'],
  true
);

export const test = {
  test() {
    // Verify that lower-cased method names are converted to upper-case.
    // even though the Fetch API doesn't do this in general for all methods.
    // Note that the upper_case_all_http_methods compat flag is intentionally
    // diverging from the Fetch API here.
    const req = new Request('https://example.com', { method: 'patch' });
    strictEqual(req.method, 'PATCH');

    // Unrecognized methods error as expected, with the error message
    // showing the original-cased method name.
    throws(() => new Request('http://example.org', { method: 'patchy' }), {
      message: /^Invalid HTTP method string: patchy$/,
    });
  },
};

export const fetchGen = {
  async test() {
    const enc = new TextEncoder();
    async function* gen() {
      yield enc.encode('Hello ');
      yield enc.encode('World!');
    }
    const resp = new Response(gen());
    strictEqual(await resp.text(), 'Hello World!');

    const req = new Request('http://example.com', {
      method: 'POST',
      body: gen(),
    });
    strictEqual(await req.text(), 'Hello World!');

    const resp2 = new Response([enc.encode('Hello '), enc.encode('World!')]);
    strictEqual(await resp2.text(), 'Hello World!');

    // Boxed strings work correctly.
    const resp3 = new Response(new String('Hello'));
    strictEqual(await resp3.text(), 'Hello');

    // Regular strings work correctly.
    const resp4 = new Response('Hello');
    strictEqual(await resp4.text(), 'Hello');
  },
};
