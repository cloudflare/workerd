// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-16:
// Verify that node:http ClientRequest rejects absolute-form and network-path
// request targets in options.path, preventing SSRF via URL authority override.

import http from 'node:http';
import { throws } from 'node:assert';

// Absolute URLs in options.path must be rejected.
// Without the fix, new URL('http://evil.test/x', baseUrl) in #onFinish
// would replace the configured host with evil.test.
export const testRejectsAbsoluteUrlPath = {
  test() {
    throws(
      () => {
        http.request({
          hostname: 'api.example.test',
          port: 80,
          path: 'http://evil.test/steal',
        });
      },
      {
        code: 'ERR_INVALID_ARG_VALUE',
      },
      'http.request must reject absolute URL in options.path'
    );
  },
};

// HTTPS absolute URLs must also be rejected.
export const testRejectsHttpsAbsoluteUrlPath = {
  test() {
    throws(
      () => {
        http.request({
          hostname: 'api.example.test',
          port: 80,
          path: 'https://evil.test/steal',
        });
      },
      {
        code: 'ERR_INVALID_ARG_VALUE',
      },
      'http.request must reject https:// absolute URL in options.path'
    );
  },
};

// Network-path references (//host/path) must be rejected.
// Without the fix, new URL('//evil.test/x', baseUrl) in #onFinish
// would replace the configured host with evil.test.
export const testRejectsNetworkPathReference = {
  test() {
    throws(
      () => {
        http.request({
          hostname: 'api.example.test',
          port: 80,
          path: '//evil.test/steal',
        });
      },
      {
        code: 'ERR_INVALID_ARG_VALUE',
      },
      'http.request must reject network-path reference in options.path'
    );
  },
};

// Cloud metadata SSRF vector must be rejected.
export const testRejectsMetadataNetworkPath = {
  test() {
    throws(
      () => {
        http.request({
          hostname: 'api.example.test',
          port: 80,
          path: '//169.254.169.254/latest/meta-data/',
        });
      },
      {
        code: 'ERR_INVALID_ARG_VALUE',
      },
      'http.request must reject metadata endpoint network-path reference'
    );
  },
};

// Backslash variants: the WHATWG URL spec normalises \ to / for special
// schemes, which would turn these into authority-overriding forms.  Our
// validation uses the same URL parser (ada-url) that later constructs the
// fetch URL, so there are two safe outcomes:
//   (a) the parser normalises \ → / and our host check catches it, OR
//   (b) the parser does NOT normalise \, so it stays in the path and
//       cannot override authority.
// Either way the fetch must never reach the attacker host.  We verify
// this by checking that the URL parser resolves these against the
// configured host without authority override.
export const testBackslashPathsCannotOverrideAuthority = {
  test() {
    const backslashPaths = [
      '\\\\evil.test/x',
      '\\/evil.test/x',
      '/\\evil.test/x',
    ];
    for (const path of backslashPaths) {
      // If the parser normalises \ to /, our check rejects it (throws).
      // If it doesn't normalise, the path is safe. Either way, verify
      // that the URL used for the fetch would never reach evil.test.
      let rejected = false;
      try {
        const req = http.request({
          hostname: 'api.example.test',
          port: 80,
          path,
        });
        req.destroy();
      } catch (e) {
        if (e.code === 'ERR_INVALID_ARG_VALUE') {
          rejected = true;
        } else {
          throw e;
        }
      }

      if (!rejected) {
        // The request was allowed — verify the URL parser keeps the
        // configured host (i.e. backslash was NOT normalised to /).
        const resolved = new URL(path, 'http://api.example.test/');
        if (resolved.host !== 'api.example.test') {
          throw new Error(
            `Backslash path "${path}" was allowed but URL parser resolved ` +
              `host to "${resolved.host}" — authority override!`
          );
        }
      }
    }
  },
};

// Defense-in-depth: mutating req.path after construction must not bypass
// the SSRF guard.  The #onFinish check should catch it and destroy the
// request with an error.
export const testRejectsPathMutationAfterConstruction = {
  async test() {
    const req = http.request({
      hostname: 'api.example.test',
      port: 80,
      path: '/safe',
    });
    // Mutate the public field to an authority-overriding value.
    req.path = '//evil.test/steal';

    const error = await new Promise((resolve) => {
      req.on('error', resolve);
      req.end();
    });

    if (error.code !== 'ERR_INVALID_ARG_VALUE') {
      throw new Error(
        `Expected ERR_INVALID_ARG_VALUE but got ${error.code}: ${error.message}`
      );
    }
  },
};

// Normal relative paths must still work.
export const testAllowsNormalPaths = {
  test() {
    // These should NOT throw — they are valid path-only request targets.
    const req1 = http.request({ hostname: 'example.test', path: '/foo/bar' });
    req1.destroy();

    const req2 = http.request({ hostname: 'example.test', path: '/foo?q=1' });
    req2.destroy();

    const req3 = http.request({ hostname: 'example.test', path: '/' });
    req3.destroy();

    const req4 = http.request({ hostname: 'example.test', path: '/foo#hash' });
    req4.destroy();

    // Path with encoded characters should work.
    const req5 = http.request({ hostname: 'example.test', path: '/foo%20bar' });
    req5.destroy();
  },
};
