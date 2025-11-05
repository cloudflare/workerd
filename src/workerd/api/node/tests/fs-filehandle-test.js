// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ok, notStrictEqual, strictEqual, rejects } from 'node:assert';

// sync and datasync are generally non-ops. The most they do is
// verify that the file descriptor is valid.

import { promises } from 'node:fs';

export const openCloseTest = {
  async test() {
    const fileHandle = await promises.open('/tmp/test.txt', 'w+');
    strictEqual(fileHandle.constructor.name, 'FileHandle');

    // If the FileHandle is opened successfully, stat should work.
    const stat = await fileHandle.stat();
    ok(stat.isFile());

    // Close the file handle
    await fileHandle.close();

    // Verify that the file handle is closed
    await rejects(fileHandle.stat(), {
      code: 'EBADF',
    });
  },
};

export const chmodChownTest = {
  async test() {
    await using fileHandle = await promises.open('/tmp/test.txt', 'w+');

    const stat = await fileHandle.stat();
    ok(stat.isFile());

    // Change the file mode
    await fileHandle.chmod(0o644);

    // Verify the mode has not changed since chmod is a no-op
    const newStat = await fileHandle.stat();
    strictEqual(newStat.mode, stat.mode);

    // Change the file ownership
    await fileHandle.chown(1000, 1000);
    // Verify the ownership has not changed since chown is a no-op
    const newStat2 = await fileHandle.stat();
    strictEqual(newStat2.uid, stat.uid);
    strictEqual(newStat2.gid, stat.gid);
  },
};

export const syncTest = {
  async test() {
    await using fileHandle = await promises.open('/tmp/test.txt', 'w+');
    strictEqual(fileHandle.constructor.name, 'FileHandle');

    // If the FileHandle is opened successfully, stat should work.
    const stat = await fileHandle.stat();
    ok(stat.isFile());

    // These are non-ops with no testable side effect. Just make
    // sure they don't reject.
    await fileHandle.sync();
    await fileHandle.datasync();
  },
};

export const writeAppendReadFileTest = {
  async test() {
    await using fileHandle = await promises.open('/tmp/test.txt', 'a+');
    strictEqual(fileHandle.constructor.name, 'FileHandle');

    await fileHandle.writeFile('Hello, World');

    // Append some data to the file
    await fileHandle.appendFile('!!!!\n');

    // Read the file back
    const data = await fileHandle.readFile('utf8');
    strictEqual(data, 'Hello, World!!!!\n');
  },
};

export const writeReadTest = {
  async test() {
    // Note that we're opening the file in append mode...
    await using fileHandle = await promises.open('/tmp/test.txt', 'a+');
    strictEqual(fileHandle.constructor.name, 'FileHandle');

    // Write some data to the file
    await fileHandle.write('Hello, World');

    // Append some data to the file
    await fileHandle.write('!!!!\n');

    // Read the file back
    const buffer = Buffer.alloc(100);
    const { bytesRead } = await fileHandle.read(buffer, 0, buffer.length, 0);
    strictEqual(bytesRead, 17);
    strictEqual(buffer.toString('utf8', 0, bytesRead), 'Hello, World!!!!\n');

    // Use readv
    const buffer2 = Buffer.alloc(10);
    const buffer3 = Buffer.alloc(10);
    const { bytesRead: bytesRead2 } = await fileHandle.readv(
      [buffer2, buffer3],
      0
    );
    strictEqual(bytesRead2, 17);
    const buffer4 = Buffer.concat([buffer2, buffer3]);
    strictEqual(buffer4.toString('utf8', 0, bytesRead2), 'Hello, World!!!!\n');

    await fileHandle.writev(
      [Buffer.from('More data'), Buffer.from(' to write\n')],
      0
    );
    // Read the file back again
    const buffer5 = Buffer.alloc(100);
    const { bytesRead: bytesRead3 } = await fileHandle.read(
      buffer5,
      0,
      buffer5.length,
      0
    );
    strictEqual(bytesRead3, 36);
    strictEqual(
      buffer5.toString('utf8', 0, bytesRead3),
      'Hello, World!!!!\nMore data to write\n'
    );
  },
};

export const truncateTest = {
  async test() {
    await using fileHandle = await promises.open('/tmp/test.txt', 'w+');
    strictEqual(fileHandle.constructor.name, 'FileHandle');

    // Write some data to the file
    await fileHandle.writeFile('Hello, World');

    // Truncate the file to 5 bytes
    await fileHandle.truncate(5);

    // Read the file back
    const data = await fileHandle.readFile('utf8');
    strictEqual(data, 'Hello');
  },
};

export const utimesTest = {
  async test() {
    await using fileHandle = await promises.open('/tmp/test.txt', 'w+');
    strictEqual(fileHandle.constructor.name, 'FileHandle');

    // If the FileHandle is opened successfully, stat should work.
    const stat = await fileHandle.stat();
    ok(stat.isFile());

    // Update the access and modification times
    const now = new Date();
    await fileHandle.utimes(now, now);

    const newStat = await fileHandle.stat();

    // atime should not be changed, mtime should.
    strictEqual(newStat.atime.getTime(), stat.atime.getTime());
    notStrictEqual(newStat.mtime.getTime(), stat.mtime.getTime());
  },
};
