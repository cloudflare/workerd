// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

// Test that TLS errors produce helpful error messages rather than opaque "internal error"
export const tlsCertificateErrorMessage = {
  async test(ctrl, env, ctx) {
    // Attempt to fetch from a server with an untrusted certificate.
    // The test server uses a self-signed cert that is NOT in the trustedCertificates list
    // for the internet service, so this should fail with a TLS error.

    try {
      await fetch(`https://localhost:${env.UNTRUSTED_TLS_PORT}/`);
      assert.fail('Expected fetch to fail due to TLS certificate error');
    } catch (err) {
      // Verify we get a helpful error message, not an opaque internal error
      assert.ok(
        !err.message.includes('internal error; reference ='),
        `Expected helpful TLS error message, got opaque internal error: ${err.message}`
      );

      // The error should mention TLS/certificate issues and be actionable
      assert.ok(
        err.message.includes('TLS') ||
          err.message.includes('certificate') ||
          err.message.includes('Network connection failed'),
        `Expected error message to be helpful about TLS/certificate issues, got: ${err.message}`
      );

      // Should mention NODE_EXTRA_CA_CERTS as the workaround
      assert.ok(
        err.message.includes('NODE_EXTRA_CA_CERTS'),
        `Expected error message to mention NODE_EXTRA_CA_CERTS workaround, got: ${err.message}`
      );
    }
  },
};
