// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request) {
    // --- FormData body with custom Content-Type (http.c++) ---
    // The custom Content-Type will lack the boundary parameter that the FormData serializer
    // generates, preventing the recipient from parsing the body.
    const formData = new FormData();
    formData.append('key', 'value');
    const req = new Request('http://example.com', {
      method: 'POST',
      body: formData,
      headers: { 'Content-Type': 'multipart/form-data' },
    });

    // --- Null-body status with zero-length body (http.c++) ---
    // Constructing a Response with a null-body status (204) and a non-null, zero-length body
    // is technically incorrect and should warn.
    const resp = new Response('', { status: 204 });

    return new Response('ok');
  },
};

export const test = {
  async test(ctrl, env) {
    // Invoke via service binding so the fetch() handler runs in a traced invocation
    // (test() handlers are intentionally not traced by the test runner).
    await env.SELF.fetch('http://dummy');
  },
};
