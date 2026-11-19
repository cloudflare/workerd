// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The webfs services of startup-snapshot-wrappables-test: the shared case plus file system handles
// (navigator.storage) the top level keeps. A handle is a locator and a name, which is all its
// snapshotClone() carries: a restored handle resolves against the VFS of the Worker it is
// restored into.
//
// webfs-tmp keeps a handle to a file the top level creates under /tmp, writes and reads back.
// The top level's temporary files do not outlive it, with or without a snapshot, so at request
// time the handle finds nothing; once the file exists again, the handle writes and reads it.
//
// webfs-bundle keeps a handle to config.txt, a text module of the Worker's config, which the VFS
// serves read-only under /bundle: it reads the same at the top level and at request time.

import { env } from 'cloudflare:workers';
import { report } from './worker.js';

const MODE = env.MODE;
const FILE_NAME = MODE === 'webfs-tmp' ? 'snap.txt' : 'config.txt';

// The File, the writable stream and its write context stay local to these helpers, so the handles
// are the only file system objects the top level leaves reachable.
async function readText(handle) {
  const file = await handle.getFile();
  return await file.text();
}

async function writeText(handle, text) {
  const writable = await handle.createWritable();
  await writable.write(text);
  await writable.close();
}

const root = await navigator.storage.getDirectory();
const tmp = await root.getDirectoryHandle('tmp');
const bundle = await root.getDirectoryHandle('bundle');
const directory = MODE === 'webfs-tmp' ? tmp : bundle;

let file;
if (MODE === 'webfs-tmp') {
  file = await tmp.getFileHandle(FILE_NAME, { create: true });
  await writeText(file, 'written at the top level');
} else {
  file = await bundle.getFileHandle(FILE_NAME);
}
const textAtTopLevel = await readText(file);

export default {
  async fetch() {
    const checks = {
      directoryHandles:
        root.kind === 'directory' &&
        root.name === '' &&
        tmp.kind === 'directory' &&
        tmp.name === 'tmp' &&
        bundle.kind === 'directory' &&
        bundle.name === 'bundle',
      fileHandle: file.kind === 'file' && file.name === FILE_NAME,
    };
    if (MODE === 'webfs-tmp') {
      checks.readAtTopLevel = textAtTopLevel === 'written at the top level';
      // A request has its own /tmp, which starts out empty.
      const stale = await file.getFile().then(
        () => 'found',
        (error) => error.name
      );
      checks.goneAtRequest = stale === 'NotFoundError';
      const created = await directory.getFileHandle(FILE_NAME, {
        create: true,
      });
      checks.sameEntry = created !== file && (await file.isSameEntry(created));
      await writeText(file, 'written at request time');
      checks.readAtRequest =
        (await readText(file)) === 'written at request time';
    } else {
      checks.readAtTopLevel = textAtTopLevel === 'hello from the config';
      checks.readAtRequest = (await readText(file)) === 'hello from the config';
      const found = await directory.getFileHandle(FILE_NAME);
      checks.sameEntry = found !== file && (await file.isSameEntry(found));
    }
    return Response.json(await report(checks));
  },
};
