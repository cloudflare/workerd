// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression tests for AUTOVULN-CLOUDFLARE-WORKERD-16, VULN-146188 and
// VULN-157700: node:http ClientRequest must not let options.path,
// options.host or options.hostname, or the path and host fields after
// construction, override the authority of the URL it sends to.

import http from 'node:http';
import { deepStrictEqual, strictEqual, throws } from 'node:assert';

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

// Replaces fetch() while `fn` runs, recording the URL of every request
// node:http sends and answering each with an empty 200 response.
async function recordFetches(fn) {
  const original = globalThis.fetch;
  const urls = [];
  globalThis.fetch = async (url) => {
    urls.push(new URL(url).href);
    return new Response(null);
  };
  try {
    await fn();
  } finally {
    globalThis.fetch = original;
  }
  return urls;
}

// Sends a request for `options`, passing it to `change` first. Resolves
// with the request's error, or its response status.
function send(options, change) {
  const req = http.request({ port: 80, ...options });
  change?.(req);
  return new Promise((resolve) => {
    req.on('error', (error) => resolve({ error }));
    req.on('response', (res) => {
      res.resume();
      resolve({ status: res.statusCode });
    });
    req.end();
  });
}

// Sends a request to api.example.test whose path is reassigned after
// construction.
function sendWithPathChangedTo(path) {
  return send({ hostname: 'api.example.test', path: '/safe' }, (req) => {
    req.path = path;
  });
}

// VULN-146188: the constructor validates options.path, but `path` is a
// public field. A path assigned afterwards must be validated the same way
// when the request is sent. The URL parser strips leading control
// characters and spaces and removes tabs and newlines anywhere, so each of
// these would otherwise send the request to evil.test.
export const testRejectsAuthorityOverridingPathSetAfterConstruction = {
  async test() {
    const cases = [
      ['//evil.test/steal', 'ERR_INVALID_ARG_VALUE'],
      ['http://evil.test/steal', 'ERR_INVALID_ARG_VALUE'],
      ['\t//evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      ['\n//evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      ['\r\n//evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      ['\u0000//evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      ['\u001f//evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      [' //evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      ['/\t/evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      ['\t\\\\evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
      ['\thttp://evil.test/steal', 'ERR_UNESCAPED_CHARACTERS'],
    ];
    for (const [path, code] of cases) {
      // Each case must be one that would reach evil.test if it got through.
      strictEqual(
        new URL(path, 'http://api.example.test/').host,
        'evil.test',
        JSON.stringify(path)
      );

      let result;
      const urls = await recordFetches(async () => {
        result = await sendWithPathChangedTo(path);
      });
      deepStrictEqual(urls, [], JSON.stringify(path));
      strictEqual(result.error?.code, code, JSON.stringify(path));
    }
  },
};

// A valid path assigned after construction is still sent, to the
// configured host. This also shows that recordFetches() sees the requests
// node:http makes, so the empty lists above mean nothing was sent.
export const testSendsValidPathSetAfterConstruction = {
  async test() {
    let result;
    const urls = await recordFetches(async () => {
      result = await sendWithPathChangedTo('/changed?q=1');
    });
    deepStrictEqual(urls, ['http://api.example.test/changed?q=1']);
    strictEqual(result.status, 200);
  },
};

// VULN-157700: node:http builds the request URL as `http://${host}`.
// Node passes options.host and options.hostname to DNS as they are, so a
// value that ends in ".api.example.test" but contains a URL delimiter fails
// there. The URL parser instead ends the host at the delimiter, so an
// allowlist that checks the suffix would let these send to evil.test.
const AUTHORITY_SPLITTING_HOSTS = [
  'evil.test/.api.example.test',
  'evil.test\\.api.example.test',
  'evil.test?.api.example.test',
  'evil.test#.api.example.test',
  'api.example.test@evil.test',
  'evil.test\t/.api.example.test',
];

export const testRejectsAuthoritySplittingHost = {
  async test() {
    for (const host of AUTHORITY_SPLITTING_HOSTS) {
      // Each case must be one that would reach evil.test if it got through.
      strictEqual(
        new URL(`http://${host}`).hostname,
        'evil.test',
        JSON.stringify(host)
      );

      for (const field of ['host', 'hostname']) {
        throws(
          () => http.request({ [field]: host, port: 80, path: '/' }),
          { code: 'ERR_INVALID_ARG_VALUE' },
          `${field}: ${JSON.stringify(host)}`
        );
      }

      // `host` is also a public field, which a caller may reassign after
      // construction.
      let result;
      const urls = await recordFetches(async () => {
        result = await send({ hostname: 'api.example.test' }, (req) => {
          req.host = host;
        });
      });
      deepStrictEqual(urls, [], JSON.stringify(host));
      strictEqual(
        result.error?.code,
        'ERR_INVALID_ARG_VALUE',
        JSON.stringify(host)
      );
    }
  },
};

// Host names and addresses, IPv6 in brackets included, are still sent to.
export const testSendsToValidHosts = {
  async test() {
    const cases = [
      [{ hostname: 'api.example.test' }, 'http://api.example.test/'],
      [{ host: 'sub.api.example.test' }, 'http://sub.api.example.test/'],
      [{ hostname: '127.0.0.1' }, 'http://127.0.0.1/'],
      [{ hostname: '[::1]' }, 'http://[::1]/'],
    ];
    for (const [options, expected] of cases) {
      let result;
      const urls = await recordFetches(async () => {
        result = await send(options);
      });
      deepStrictEqual(urls, [expected]);
      strictEqual(result.status, 200);
    }
  },
};

// Normal relative paths must still work.
export const testAllowsNormalPaths = {
  test() {
    // These should NOT throw — they are valid path-only request targets.
    // Each request is destroyed unsent, which reports 'socket hang up'.
    for (const path of [
      '/foo/bar',
      '/foo?q=1',
      '/',
      '/foo#hash',
      '/foo%20bar',
    ]) {
      const req = http.request({ hostname: 'example.test', path });
      req.on('error', () => {});
      req.destroy();
    }
  },
};
