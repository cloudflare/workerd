// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// fetch() rejects a network error with a TypeError under fetch_invalid_response_type_error, and
// with an Error otherwise. The message is the same either way.

import assert from 'node:assert';

export const invalidResponse = {
  async test(ctrl, env) {
    await assert.rejects(
      fetch(
        `http://${env.SIDECAR_HOSTNAME}:${env.INVALID_RESPONSE_SERVER_PORT}/`
      ),
      {
        name: env.EXPECTED_ERROR,
        message:
          /^Server returned invalid HTTP response: The headers sent by the server are not valid\./,
      }
    );
  },
};

export const hostThatDoesNotResolve = {
  async test(ctrl, env) {
    // The cause is the OS's DNS error, which each platform words differently.
    await assert.rejects(fetch('http://does-not-exist.invalid/'), {
      name: env.EXPECTED_ERROR,
      message: /^Network connection failed: ./,
    });
  },
};

export const untrustedCertificate = {
  async test(ctrl, env) {
    await assert.rejects(
      fetch(
        `https://${env.SIDECAR_HOSTNAME}:${env.UNTRUSTED_TLS_SERVER_PORT}/`
      ),
      {
        name: env.EXPECTED_ERROR,
        message:
          /^Network connection failed: TLS handshake failed: invalid peer certificate/,
      }
    );
  },
};
