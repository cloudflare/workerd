// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for heap-use-after-free in MessagePort.postMessage().
// A custom getter during serialization can close + GC the target port,
// leaving a dangling reference inside the runIfAlive lambda.
export const closeAndGcDuringPostMessage = {
  test() {
    let port1;
    (() => {
      const { port1: p1, port2: _p2 } = new MessageChannel();
      port1 = p1;
      // port2 (_p2) goes out of scope here — only reachable via port1's weak ref.
    })();

    const maliciousObject = {};
    Object.defineProperty(maliciousObject, 'value', {
      get() {
        port1.close();
        for (let i = 0; i < 50; i++) gc();
        return 42;
      },
      enumerable: true,
    });

    // Should not crash even though the getter closes the port and forces GC.
    port1.postMessage(maliciousObject);
  },
};
