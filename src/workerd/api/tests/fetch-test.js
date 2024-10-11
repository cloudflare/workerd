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
