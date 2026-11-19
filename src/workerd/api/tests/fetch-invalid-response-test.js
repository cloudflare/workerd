// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// fetch() of a server whose response head doesn't parse rejects with a TypeError under
// fetch_invalid_response_type_error, and with an Error otherwise. The message is the same.

import assert from 'node:assert';

export const invalidResponse = {
  async test(ctrl, env) {
    const url = `http://${env.SIDECAR_HOSTNAME}:${env.INVALID_RESPONSE_SERVER_PORT}/`;
    const error = await fetch(url).then(
      () => assert.fail('fetch() should reject'),
      (e) => e
    );
    assert.strictEqual(error.constructor.name, env.EXPECTED_ERROR);
    assert.match(
      error.message,
      /^Server returned invalid HTTP response: The headers sent by the server are not valid\./
    );
  },
};
