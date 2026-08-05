// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects } from 'node:assert';

export const test = {
  async test() {
    // This config sets throw_on_unrecognized_import_assertion, so the import
    // is rejected under both module registries; only the message differs
    // (legacy: 'Unrecognized import attributes', new registry: 'Unsupported
    // import attribute: "a"'). Note the flag itself applies only to the
    // legacy registry — the new registry always rejects unknown attributes.
    await rejects(import('worker', { with: { a: 'b' } }), {
      message: /Unrecognized import attributes|Unsupported import attribute/,
    });
  },
};
