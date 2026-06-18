// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-33:
// node:tls silently accepted and ignored the checkServerIdentity option,
// creating a false sense of security for applications relying on certificate
// pinning or custom hostname verification. The fix rejects the option with
// ERR_OPTION_NOT_IMPLEMENTED until getPeerCertificate() is available to
// actually invoke the verifier.

import tls from 'node:tls';
import { throws, doesNotThrow } from 'node:assert';

// Verify that providing a custom checkServerIdentity to tls.connect() throws
// ERR_OPTION_NOT_IMPLEMENTED, rather than silently ignoring the verifier.
export const regressionCheckServerIdentityConnect = {
  test() {
    throws(
      () => {
        tls.connect({
          port: 443,
          host: 'example.com',
          checkServerIdentity(hostname, cert) {
            return new Error('pin mismatch');
          },
        });
      },
      {
        code: 'ERR_OPTION_NOT_IMPLEMENTED',
      },
      'tls.connect() with custom checkServerIdentity must throw ERR_OPTION_NOT_IMPLEMENTED'
    );
  },
};

// Verify that providing a custom checkServerIdentity to the TLSSocket
// constructor also throws ERR_OPTION_NOT_IMPLEMENTED.
export const regressionCheckServerIdentityTLSSocket = {
  test() {
    throws(
      () => {
        new tls.TLSSocket(undefined, {
          checkServerIdentity(hostname, cert) {
            return new Error('pin mismatch');
          },
        });
      },
      {
        code: 'ERR_OPTION_NOT_IMPLEMENTED',
      },
      'new TLSSocket() with custom checkServerIdentity must throw ERR_OPTION_NOT_IMPLEMENTED'
    );
  },
};

// Verify that passing a non-function value (e.g. a number) as
// checkServerIdentity throws a TypeError from validateFunction, which is
// a distinct error path from the ERR_OPTION_NOT_IMPLEMENTED thrown for
// actual function values.
export const nonFunctionCheckServerIdentityConnect = {
  test() {
    throws(
      () => {
        tls.connect({
          port: 443,
          host: 'example.com',
          checkServerIdentity: 42,
        });
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      },
      'tls.connect() with non-function checkServerIdentity must throw TypeError'
    );
  },
};

export const nonFunctionCheckServerIdentityTLSSocket = {
  test() {
    throws(
      () => {
        new tls.TLSSocket(undefined, {
          checkServerIdentity: 42,
        });
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      },
      'new TLSSocket() with non-function checkServerIdentity must throw TypeError'
    );
  },
};

// Verify that omitting checkServerIdentity (the common case) still works —
// the default built-in checkServerIdentity is used internally and must not
// trigger the rejection.
export const regressionCheckServerIdentityDefault = {
  test() {
    doesNotThrow(() => {
      // connect with lookup stub so we don't actually open a connection
      tls.connect({ port: 42, lookup() {} });
    }, 'tls.connect() without custom checkServerIdentity must not throw');
  },
};
