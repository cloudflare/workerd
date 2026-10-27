// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// HTTP methods outside kj's list, with the OTHER_HTTP_METHODS autogate enabled by this test's
// config. request-other-methods-disabled-test.js covers the gate being disabled.

import { strictEqual, throws } from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

// The config turns upper_case_all_http_methods off: with it on, every spelling of a listed method
// is uppercased before anything here applies (see fetch-test.js).
strictEqual(
  globalThis.Cloudflare.compatibilityFlags['upper_case_all_http_methods'],
  false
);

export class MethodEcho extends WorkerEntrypoint {
  methodOf(request) {
    return request.method;
  }
}

export default {
  fetch(request) {
    return new Response(request.method);
  },
};

export const constructor = {
  test() {
    // A valid method outside kj's list is kept exactly as written.
    strictEqual(
      new Request('http://example.org', { method: 'BLOCK' }).method,
      'BLOCK'
    );
    strictEqual(
      new Request('http://example.org', { method: 'block' }).method,
      'block'
    );

    // Another spelling of a method in kj's list is a method of its own too, kept as written, so
    // that adding a method to kj's list doesn't change how its other spellings behave. Only the
    // six names the Fetch spec normalizes are uppercased.
    for (const method of ['patch', 'Purge', 'propFind']) {
      strictEqual(new Request('http://example.org', { method }).method, method);
    }
    strictEqual(
      new Request('http://example.org', { method: 'get' }).method,
      'GET'
    );

    // The Fetch spec forbids CONNECT, TRACE and TRACK, in any letter case...
    for (const method of [
      'CONNECT',
      'connect',
      'TRACK',
      'Track',
      'trace',
      'Trace',
    ]) {
      throws(() => new Request('http://example.org', { method }), {
        name: 'TypeError',
        message: `Invalid HTTP method string: ${method}`,
      });
    }

    // ...but exactly "TRACE" is in kj's list and stays accepted, so that no existing Worker breaks.
    strictEqual(
      new Request('http://example.org', { method: 'TRACE' }).method,
      'TRACE'
    );

    // kj::HttpMethod holds names of up to 22 characters.
    const longest = 'X'.repeat(22);
    strictEqual(
      new Request('http://example.org', { method: longest }).method,
      longest
    );
    const tooLong = 'X'.repeat(23);
    throws(() => new Request('http://example.org', { method: tooLong }), {
      name: 'TypeError',
      message: `Invalid HTTP method string: ${tooLong}`,
    });

    // Invalid methods are still rejected.
    for (const method of ['BL OCK', '', 'BLOCK\r\nX-Injected: 1']) {
      throws(() => new Request('http://example.org', { method }), {
        name: 'TypeError',
        message: `Invalid HTTP method string: ${method}`,
      });
    }
  },
};

export const fetchSendsMethod = {
  async test(ctrl, env) {
    for (const method of ['BLOCK', 'block', 'patch']) {
      const response = await env.SELF.fetch('http://example.org/', { method });
      strictEqual(await response.text(), method);
    }
  },
};

export const rpcCarriesMethod = {
  async test(ctrl, env) {
    const request = new Request('http://example.org/', { method: 'BLOCK' });
    strictEqual(await env.ECHO.methodOf(request), 'BLOCK');
  },
};
