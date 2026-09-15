// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

export const datagramGlobal = {
  test(_ctrl, env) {
    strictEqual('Datagram' in globalThis, env.EXPERIMENTAL);
    if (env.EXPERIMENTAL) {
      const { Datagram } = globalThis;
      strictEqual(typeof Datagram, 'function');
      const data = new Uint8Array([1, 2, 3]);
      const datagram = new Datagram(data);
      strictEqual(datagram instanceof Datagram, true);
      strictEqual(datagram.data, data);
    }
  },
};
