// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Buffer } from 'node:buffer';

const root = await navigator.storage.getDirectory();
const tmp = await root.getDirectoryHandle('tmp');
export default {
  async fetch(req, env) {

    // Any files that are created in the tmp file directory will be
    // automatically cleaned up by the runtime after each request.
    // The tmp dir is specific to each request, so concurrent requests
    // will not interfere with each other and there is no shared state
    // between requests.

    // Web File System APIs...
    const file = await tmp.getFileHandle("foo", { create: true });
    const writable = await file.createWritable();
    await writable.write('Hello World\n');
    await writable.close();

    // Node.js File System APIs...
    const { readFileSync } = await import('node:fs');
    const data = readFileSync('/tmp/foo', 'utf8');

    return new Response(data);
  }
};
