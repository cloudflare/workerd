// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects } from 'node:assert';

export const test = {
  async test() {
    await rejects(import('worker', { with: { a: 'b' } }), {
      message: /Unrecognized import attributes/,
    });
  },
};
