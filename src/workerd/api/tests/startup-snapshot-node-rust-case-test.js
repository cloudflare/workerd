// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-node-test.js: Node.js modules whose native side is a Rust resource.
// Importing them leaves the resource reachable from the top level.

import dns from 'node:dns';
import url from 'node:url';
import unsafe from 'workerd:unsafe';

export default {
  async fetch() {
    return Response.json({
      mode: 'rust-modules',
      snapshotUsed: unsafe.startedFromStartupSnapshot(),
      checks: {
        url: url.domainToASCII('español.com') === 'xn--espaol-zwa.com',
        urlUnicode: url.domainToUnicode('xn--espaol-zwa.com') === 'español.com',
        dns: typeof dns.lookup === 'function',
      },
    });
  },
};
