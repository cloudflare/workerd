// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A user-constructed MessageEvent accepts ports as its source and ports,
// and reflects them. (The runtime never attaches ports: they are not
// transferable here.)

import { deepStrictEqual, strictEqual } from 'node:assert';

export const userConstructedEventReflectsPorts = {
  test() {
    const { port1, port2 } = new MessageChannel();
    const event = new MessageEvent('message', {
      data: 'x',
      source: port1,
      ports: [port1, port2],
    });
    strictEqual(event.source, port1);
    deepStrictEqual(event.ports, [port1, port2]);
    strictEqual(event.isTrusted, false);
    strictEqual(event.origin, '');
    port1.close();
  },
};
