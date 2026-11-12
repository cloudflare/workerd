// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The fs service of startup-snapshot-wrappables-test: the shared case plus node:fs, whose module
// object (FileSystemModule) carries its temporary-name counter through the snapshot. A directory
// made at the top level and one made at request time must get the names a plain Worker gives
// them, so a name minted after the restore cannot collide with one the top level saved. Today the
// case starts as a plain Worker (see the runner), so the counter check only pins the names down.

import fs from 'node:fs';
import { report } from './worker.js';

const tmpAtTopLevel = fs.mkdtempSync('/tmp/snap-');

export default {
  async fetch() {
    const tmpAtRequest = fs.mkdtempSync('/tmp/snap-');
    return Response.json(
      await report({
        tmpAtTopLevel: tmpAtTopLevel === '/tmp/snap-0',
        tmpAtRequest: tmpAtRequest === '/tmp/snap-1',
      })
    );
  },
};
