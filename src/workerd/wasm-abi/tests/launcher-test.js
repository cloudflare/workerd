// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

// Every bundle runs once with the legacy module registry and once with the new one; the
// bindings to them end in the registry's name.
const REGISTRIES = ['LEGACY', 'NEW'];

async function probe(service) {
  return (await service.fetch('http://probe/')).json();
}

// The launcher loads the shim modules for cf:core/v1 and cf:http/v1 through import() with
// computed specifiers, and the shim serves the guest.
export const servesThroughTheLauncher = {
  async test(ctrl, env) {
    for (const registry of REGISTRIES) {
      const response = await env[`GUEST_${registry}`].fetch('http://guest/');
      assert.strictEqual(response.status, 200, registry);
      assert.strictEqual(response.headers.get('x-guest'), 'wasm', registry);
      assert.strictEqual(response.headers.get('x-method'), 'GET', registry);
      assert.strictEqual(await response.text(), 'hello from wasm', registry);
    }
  },
};

// A bundle that lacks the shim module of an import module its guest uses fails to load, with
// an error naming the import module.
export const missingShimModule = {
  async test(ctrl, env) {
    for (const registry of REGISTRIES) {
      assert.deepStrictEqual(await probe(env[`MISSING_SHIM_${registry}`]), {
        name: 'Error',
        message:
          'cannot load ./cf-http-v1.js, the shim module of Wasm import module cf:http/v1',
        hasCause: true,
      });
    }
  },
};

// A guest that imports from a module outside the ABI fails to load, with an error naming it.
export const foreignImportModule = {
  async test(ctrl, env) {
    for (const registry of REGISTRIES) {
      assert.deepStrictEqual(await probe(env[`FOREIGN_MODULE_${registry}`]), {
        name: 'TypeError',
        message:
          'Wasm import module env is not a module of the Workers Wasm ABI',
        hasCause: false,
      });
    }
  },
};
