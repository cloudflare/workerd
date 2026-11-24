// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Listing directories of the virtual file system: node:fs readdir() and
// cp(), and the web File System API's directory iteration.

import {
  cpSync,
  existsSync,
  mkdirSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';

// Fills `dir` with `count` files, unless it already exists. Called from each
// case's untimed setup, since the contents of /tmp belong to a request.
function ensureFlat(dir, count) {
  if (existsSync(dir)) return;
  mkdirSync(dir, { recursive: true });
  for (let i = 0; i < count; i++) {
    writeFileSync(`${dir}/file-${i}.txt`, 'x');
  }
}

// `dirs` directories of `files` files each.
function ensureTree(dir, dirs, files) {
  if (existsSync(dir)) return;
  for (let d = 0; d < dirs; d++) {
    ensureFlat(`${dir}/dir-${d}`, files);
  }
}

export default {
  bench(b) {
    for (const count of [10, 1000, 10000]) {
      const dir = `/tmp/flat-${count}`;
      b.run(`readdirSync ${count}`, () => b.blackBox(readdirSync(dir)), {
        setup: () => ensureFlat(dir, count),
      });
    }

    b.run(
      'readdirSync withFileTypes 1000',
      () => b.blackBox(readdirSync('/tmp/flat-1000', { withFileTypes: true })),
      { setup: () => ensureFlat('/tmp/flat-1000', 1000) }
    );

    b.run(
      'readdirSync recursive 10x100',
      () => b.blackBox(readdirSync('/tmp/tree', { recursive: true })),
      { setup: () => ensureTree('/tmp/tree', 10, 100) }
    );

    // cp() lists the source directory; the copy is removed in the same call so
    // that every call copies into an empty destination.
    b.run(
      'cpSync+rmSync 100',
      () => {
        cpSync('/tmp/flat-100', '/tmp/flat-100-copy', { recursive: true });
        rmSync('/tmp/flat-100-copy', { recursive: true });
      },
      { setup: () => ensureFlat('/tmp/flat-100', 100) }
    );

    b.run(
      'FileSystemDirectoryHandle.keys() 1000',
      async () => {
        const root = await navigator.storage.getDirectory();
        const tmp = await root.getDirectoryHandle('tmp');
        const dir = await tmp.getDirectoryHandle('flat-1000');
        const names = [];
        for await (const name of dir.keys()) names.push(name);
        b.blackBox(names);
      },
      { setup: () => ensureFlat('/tmp/flat-1000', 1000) }
    );
  },
};
