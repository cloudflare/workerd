// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ok, strictEqual, deepStrictEqual, rejects } from 'node:assert';

ok(navigator.storage instanceof StorageManager);

const dir = await navigator.storage.getDirectory();
ok(dir instanceof FileSystemDirectoryHandle);
strictEqual(dir.name, '');

const bundle = await dir.getDirectoryHandle('bundle');
const temp = await dir.getDirectoryHandle('tmp');

strictEqual(bundle.name, 'bundle');
strictEqual(temp.name, 'tmp');

let count = 0;
for await (const node of dir) {
  // The node is an Array of [name, handle]
  strictEqual(typeof node[0], 'string');
  ok(node[1] instanceof FileSystemDirectoryHandle);
  count++;
}
strictEqual(count, 3);

const names = await Array.fromAsync(dir.keys());
deepStrictEqual(names, ['bundle', 'tmp', 'dev']);

export const webfsTest = {
  async test() {
    const file = await temp.getFileHandle('foo.txt', { create: true });
    ok(file instanceof FileSystemFileHandle);
    const writable = await file.createWritable();
    ok(writable instanceof FileSystemWritableFileStream);
    const enc = new TextEncoder();
    await writable.write(enc.encode('hello world'));

    // The original file contents remain unchanged until the writable is closed.
    let f = await file.getFile();
    strictEqual(await f.text(), '');
    await writable.close();

    // Once closed, however, the file contents are updated.
    f = await file.getFile();
    strictEqual(await f.text(), 'hello world');

    const file2 = await temp.getFileHandle('foo.txt');
    ok(file2 instanceof FileSystemFileHandle);
    const fileStream = await file2.getFile();
    ok(fileStream instanceof File);
    const text = await fileStream.text();
    strictEqual(text, 'hello world');

    await rejects(temp.getFileHandle('does-not-exist.txt', { create: false }), {
      message: 'Not found',
    });
  },
};

// A write command whose `position` is past the end of the file zero-pads up to
// that position. What it must NOT do is resize the file down to the current
// cursor first, which discarded everything between the cursor and the old EOF.
//
// Each case below seeds 50 bytes and leaves the cursor at 10, so the bytes in
// 10..50 are the ones a regression destroys. The `position` is read before the
// command type is dispatched, so all three commands are affected.
async function seededWritable(name) {
  const file = await temp.getFileHandle(name, { create: true });
  const seed = await file.createWritable();
  await seed.write('A'.repeat(50));
  await seed.close();

  const writable = await file.createWritable({ keepExistingData: true });
  await writable.seek(10);
  return { file, writable };
}

async function bytesOf(file) {
  return new Uint8Array(await (await file.getFile()).arrayBuffer());
}

// slice() rather than subarray() so the comparands are byteOffset-0 copies.
const fiftyAs = new Uint8Array(50).fill(0x41);

export const writeParamsPositionPastEof = {
  async test() {
    const { file, writable } = await seededWritable('past-eof-write.txt');
    await writable.write({ type: 'write', position: 200, data: 'X' });
    await writable.close();

    const bytes = await bytesOf(file);
    strictEqual(bytes.length, 201);
    deepStrictEqual(bytes.slice(0, 50), fiftyAs);
    deepStrictEqual(bytes.slice(50, 200), new Uint8Array(150));
    strictEqual(bytes[200], 0x58);
  },
};

export const seekParamsPositionPastEof = {
  async test() {
    const { file, writable } = await seededWritable('past-eof-seek.txt');
    await writable.write({ type: 'seek', position: 200 });
    await writable.close();

    const bytes = await bytesOf(file);
    strictEqual(bytes.length, 200);
    deepStrictEqual(bytes.slice(0, 50), fiftyAs);
    deepStrictEqual(bytes.slice(50), new Uint8Array(150));
  },
};

export const truncateParamsPositionPastEof = {
  async test() {
    const { file, writable } = await seededWritable('past-eof-truncate.txt');
    await writable.write({ type: 'truncate', size: 100, position: 200 });
    await writable.close();

    const bytes = await bytesOf(file);
    strictEqual(bytes.length, 100);
    deepStrictEqual(bytes.slice(0, 50), fiftyAs);
    deepStrictEqual(bytes.slice(50), new Uint8Array(50));
  },
};

// TODO(node-fs): Rework this test now that createSyncAccessHandle has been removed.
// We can still test this using the the stream API but it needs to be structured
// a little differently.
// export const deviceTest = {
//   async test() {
//     // Test /dev/null
//     const dev = await dir.getDirectoryHandle('dev');
//     const devNull = await dev.getFileHandle('null');
//     ok(devNull instanceof FileSystemFileHandle);
//     const sync = await devNull.createSyncAccessHandle();
//     const enc = new TextEncoder();
//     strictEqual(sync.getSize(), 0);
//     sync.write(enc.encode('hello world'));
//     strictEqual(sync.getSize(), 0);
//     strictEqual(sync.read(new Uint8Array(11)), 0);

//     // Test /dev/zero
//     const devZero = await dev.getFileHandle('zero');
//     ok(devZero instanceof FileSystemFileHandle);
//     const syncZero = await devZero.createSyncAccessHandle();
//     strictEqual(syncZero.getSize(), 0);
//     syncZero.write(enc.encode('hello world'));
//     strictEqual(syncZero.getSize(), 0);
//     const u8 = new Buffer.from([1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]);
//     const zeroes = Buffer.from([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
//     strictEqual(syncZero.read(u8), u8.byteLength);
//     deepStrictEqual(u8, zeroes);

//     // Test /dev/full
//     const devFull = await dev.getFileHandle('full');
//     ok(devFull instanceof FileSystemFileHandle);
//     const syncFull = await devFull.createSyncAccessHandle();
//     strictEqual(syncFull.getSize(), 0);
//     throws(() => syncFull.write(enc.encode('hello world')), {
//       message: 'Operation not permitted',
//       name: 'NotAllowedError',
//     });
//     strictEqual(syncFull.getSize(), 0);
//     const u8Full = new Buffer.from([1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]);
//     strictEqual(syncFull.read(u8Full), 11);
//     deepStrictEqual(u8Full, zeroes);

//     // Test /dev/random
//     const devRandom = await dev.getFileHandle('random');
//     ok(devRandom instanceof FileSystemFileHandle);
//     const syncRandom = await devRandom.createSyncAccessHandle();
//     strictEqual(syncRandom.getSize(), 0);
//     throws(() => syncRandom.write(enc.encode('hello world')), {
//       message: 'Operation not permitted',
//       name: 'NotAllowedError',
//     });
//     strictEqual(syncRandom.getSize(), 0);
//     const u8Random = new Buffer.from([1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]);
//     const check = new Buffer.from([1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]);
//     strictEqual(syncRandom.read(u8Random), 11);
//     notDeepStrictEqual(u8Random, check);
//   },
// };

export const asyncIteratorTest = {
  async test() {
    const dir = await navigator.storage.getDirectory();

    let count = 0;
    for await (const node of dir) {
      // The node is an Array of [name, handle]
      ok(node[1] instanceof FileSystemDirectoryHandle);
      count++;
    }
    strictEqual(count, 3);

    const names = await Array.fromAsync(dir.keys());
    deepStrictEqual(names, ['bundle', 'tmp', 'dev']);
  },
};
