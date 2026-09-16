// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  ifError,
  ok,
  rejects,
  deepStrictEqual,
  strictEqual,
  notStrictEqual,
  throws,
  doesNotThrow,
} from 'node:assert';
import zlib from 'node:zlib';
import { Readable } from 'node:stream';

import {
  existsSync,
  statSync,
  openSync,
  closeSync,
  fstatSync,
  ftruncateSync,
  truncateSync,
  writeSync,
  writevSync,
  readSync,
  readvSync,
  readFileSync,
  writeFileSync,
  appendFileSync,
  copyFileSync,
  renameSync,
  close,
  fstat,
  ftruncate,
  truncate,
  unlinkSync,
  write,
  writev,
  writeFile,
  appendFile,
  read,
  readdir,
  readlink,
  realpath,
  mkdtemp,
  ReadStream,
  WriteStream,
  readdirSync,
  readlinkSync,
  readv,
  readFile,
  rename,
  copyFile,
  realpathSync,
  mkdtempSync,
  constants,
  promises,
  createWriteStream,
  createReadStream,
} from 'node:fs';

import { join } from 'node:path';

const { COPYFILE_EXCL } = constants;

const kErrInvalidArgType = { code: 'ERR_INVALID_ARG_TYPE' };
const kErrInvalidArgValue = { code: 'ERR_INVALID_ARG_VALUE' };
const kErrEBadf = { code: 'EBADF' };
const kErrEExist = { code: 'EEXIST' };
const kErrOutOfRange = { code: 'ERR_OUT_OF_RANGE' };

export const openCloseTest = {
  async test() {
    throws(() => fstatSync(123), kErrEBadf);
    throws(() => fstatSync(123, { bigint: 'yes' }), kErrInvalidArgType);
    throws(() => fstatSync('abc'), kErrInvalidArgType);

    // Test that all the mode combinations work
    const modes = [
      'r',
      'r+',
      'w',
      'w+',
      'a',
      'a+',
      'rs',
      'rs+',
      'wx',
      'wx+',
      'ax',
      'ax+',
    ];
    for (const mode of modes) {
      // Modes without O_CREAT need an existing file.
      if (!/[wa]/.test(mode)) writeFileSync('/tmp/test.txt', '');
      const fd = openSync('/tmp/test.txt', mode);
      ok(existsSync('/tmp/test.txt'));
      const stat = fstatSync(fd, { bigint: true });
      ok(stat);
      unlinkSync('/tmp/test.txt');
    }

    ok(!existsSync('/tmp/test.txt'));
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    // Check the exclusive option fails when the file already exists
    throws(() => openSync('/tmp/test.txt', 'wx'), kErrEExist);
    throws(() => openSync('/tmp/test.txt', 'wx+'), kErrEExist);
    throws(() => openSync('/tmp/test.txt', 'ax'), kErrEExist);
    throws(() => openSync('/tmp/test.txt', 'ax+'), kErrEExist);
    throws(() => openSync(Buffer.from('/tmp/test.txt'), 'wx'), kErrEExist);
    throws(() => openSync(Buffer.from('/tmp/test.txt'), 'wx+'), kErrEExist);
    throws(() => openSync(Buffer.from('/tmp/test.txt'), 'ax'), kErrEExist);
    throws(() => openSync(Buffer.from('/tmp/test.txt'), 'ax+'), kErrEExist);

    const stat = fstatSync(fd, { bigint: true });
    ok(stat);
    ok(stat.isFile());
    ok(!stat.isDirectory());
    strictEqual(stat.size, 0n);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fstat(fd, (err, stat) => {
        try {
          ifError(err);
          ok(stat);
          ok(stat.isFile());
          ok(!stat.isDirectory());
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    // Close the file
    closeSync(fd);
    // Can close multiple times
    closeSync(fd);
    // Can close non-existent file descriptors
    closeSync(123);

    ['a', {}, null].forEach((i) => {
      throws(() => closeSync(i), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
      throws(() => close(i), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
      throws(() => close(0, i), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
    });

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      close(fd, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }
  },
};

export const openFlagsTest = {
  test() {
    const { O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND, O_EXCL } = constants;

    // O_TRUNC truncates an existing file, both as a string flag and numeric.
    for (const flags of ['w', 'w+', O_WRONLY | O_CREAT | O_TRUNC]) {
      writeFileSync('/tmp/test.txt', 'hello world long');
      const fd = openSync('/tmp/test.txt', flags);
      strictEqual(fstatSync(fd).size, 0);
      writeSync(fd, Buffer.from('hi'));
      closeSync(fd);
      strictEqual(readFileSync('/tmp/test.txt', 'utf8'), 'hi');
    }

    // Without O_TRUNC, existing content is preserved.
    writeFileSync('/tmp/test.txt', 'hello world long');
    {
      const fd = openSync('/tmp/test.txt', O_WRONLY | O_CREAT);
      writeSync(fd, Buffer.from('hi'));
      closeSync(fd);
      strictEqual(readFileSync('/tmp/test.txt', 'utf8'), 'hillo world long');
    }

    // Numeric O_APPEND appends regardless of position.
    writeFileSync('/tmp/test.txt', 'hello');
    {
      const fd = openSync('/tmp/test.txt', O_WRONLY | O_CREAT | O_APPEND);
      writeSync(fd, Buffer.from(' world'), 0, 6, 0);
      closeSync(fd);
      strictEqual(readFileSync('/tmp/test.txt', 'utf8'), 'hello world');
    }

    // Numeric O_EXCL with O_CREAT fails on an existing file.
    throws(
      () => openSync('/tmp/test.txt', O_WRONLY | O_CREAT | O_EXCL),
      kErrEExist
    );
    throws(
      () => openSync('/tmp/test.txt', O_RDWR | O_CREAT | O_EXCL | O_TRUNC),
      kErrEExist
    );
    strictEqual(readFileSync('/tmp/test.txt', 'utf8'), 'hello world');

    // O_TRUNC on a read-only open is ignored.
    {
      const fd = openSync('/tmp/test.txt', O_TRUNC);
      closeSync(fd);
      strictEqual(readFileSync('/tmp/test.txt', 'utf8'), 'hello world');
    }

    unlinkSync('/tmp/test.txt');
  },
};

export const openMissingTest = {
  async test() {
    const { O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_EXCL, O_TRUNC } = constants;
    const kErrENoEnt = {
      code: 'ENOENT',
      syscall: 'open',
      path: '/tmp/missing',
    };

    // Without O_CREAT, opening a missing path is ENOENT and creates nothing.
    for (const flags of [
      'r',
      'r+',
      'rs+',
      O_RDONLY,
      O_WRONLY,
      O_RDWR,
      O_RDWR | O_TRUNC,
    ]) {
      throws(() => openSync('/tmp/missing', flags), kErrENoEnt);
      ok(!existsSync('/tmp/missing'));
    }

    // With O_CREAT, the file is created.
    for (const flags of [
      'w',
      'a',
      'w+',
      O_WRONLY | O_CREAT,
      O_RDONLY | O_CREAT,
    ]) {
      const fd = openSync('/tmp/missing', flags);
      closeSync(fd);
      ok(existsSync('/tmp/missing'));
      strictEqual(readFileSync('/tmp/missing', 'utf8'), '');

      // O_CREAT|O_EXCL fails once it exists, regardless of access mode.
      throws(
        () => openSync('/tmp/missing', O_RDONLY | O_CREAT | O_EXCL),
        kErrEExist
      );
      throws(() => openSync('/tmp/missing', 'wx'), kErrEExist);
      unlinkSync('/tmp/missing');
    }

    // O_CREAT|O_EXCL on a missing path creates it.
    closeSync(openSync('/tmp/missing', O_WRONLY | O_CREAT | O_EXCL));
    ok(existsSync('/tmp/missing'));
    unlinkSync('/tmp/missing');

    // O_CREAT does not create intermediate directories.
    for (const flags of ['r', 'w', 'a', O_WRONLY | O_CREAT | O_EXCL]) {
      throws(() => openSync('/tmp/missing/file', flags), {
        code: 'ENOENT',
        syscall: 'open',
        path: '/tmp/missing/file',
      });
      ok(!existsSync('/tmp/missing'));
    }

    // A non-directory path component is ENOTDIR.
    writeFileSync('/tmp/missing', '');
    for (const flags of ['r', 'w']) {
      throws(() => openSync('/tmp/missing/file', flags), {
        code: 'ENOTDIR',
        syscall: 'open',
        path: '/tmp/missing/file',
      });
    }
    strictEqual(readFileSync('/tmp/missing', 'utf8'), '');
    unlinkSync('/tmp/missing');

    // Streams open with their own flags: WriteStream defaults to 'w' and
    // honours 'a'; ReadStream defaults to 'r' and fails on a missing path.
    const writeAll = (path, data, options) =>
      new Promise((resolve, reject) => {
        createWriteStream(path, options)
          .on('close', resolve)
          .on('error', reject)
          .end(data);
      });
    await writeAll('/tmp/missing', 'first');
    strictEqual(readFileSync('/tmp/missing', 'utf8'), 'first');
    await writeAll('/tmp/missing', 'second');
    strictEqual(readFileSync('/tmp/missing', 'utf8'), 'second');
    await writeAll('/tmp/missing', ' third', { flags: 'a' });
    strictEqual(readFileSync('/tmp/missing', 'utf8'), 'second third');
    unlinkSync('/tmp/missing');

    const streamError = (make) =>
      new Promise((resolve, reject) => {
        make().on('open', resolve).on('error', reject);
      });
    await rejects(
      streamError(() => createReadStream('/tmp/missing')),
      kErrENoEnt
    );
    ok(!existsSync('/tmp/missing'));

    // Invalid flags are validated synchronously by fs.open and surface as
    // the stream error rather than a hang.
    await rejects(
      streamError(() => createReadStream('/tmp/missing', { flags: 'zz' })),
      { code: 'ERR_INVALID_ARG_VALUE' }
    );
    await rejects(
      streamError(() => createWriteStream('/tmp/missing', { flags: 'zz' })),
      { code: 'ERR_INVALID_ARG_VALUE' }
    );
    ok(!existsSync('/tmp/missing'));
  },
};

export const ftruncateTest = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    throws(() => ftruncateSync('hello'), kErrInvalidArgType);
    throws(() => ftruncateSync(123), kErrEBadf);
    throws(() => ftruncateSync(123, 10), kErrEBadf);
    throws(() => ftruncateSync(fd, 'hello'), kErrInvalidArgType);

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    // Truncate to 10 bytes
    ftruncateSync(fd, 10);
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 10n);

    // Truncate to 0 bytes
    ftruncateSync(fd, 0);
    const stat3 = fstatSync(fd, { bigint: true });
    strictEqual(stat3.size, 0n);

    // Truncate to a negative size throws an error
    throws(() => ftruncateSync(fd, -1), kErrOutOfRange);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      ftruncate(fd, 5, (err) => {
        if (err) return reject(err);
        const stat4 = fstatSync(fd, { bigint: true });
        strictEqual(stat4.size, 5n);
        resolve();
      });
      await promise;
    }

    {
      throws(() => ftruncate(fd, -1, () => {}), {
        code: 'ERR_OUT_OF_RANGE',
      });
    }

    {
      throws(() => ftruncateSync(fd, 0xffffffff), {
        message: /File size limit exceeded/,
      });
      throws(() => ftruncateSync(fd, 0x08000000 + 1), {
        message: /File size limit exceeded/,
      });
      // 0x08000000 is the maximum allowed file size.
      ftruncateSync(fd, 0x08000000);
    }

    closeSync(fd);
  },
};

export const truncateTest = {
  async test() {
    throws(() => truncateSync(123), kErrInvalidArgType);
    throws(() => truncateSync('/', 'hello'), kErrInvalidArgType);

    ok(!existsSync('/tmp/test.txt'));
    throws(() => truncateSync('/tmp/test.txt', 10), {
      code: 'ENOENT',
      path: '/tmp/test.txt',
    });

    // Create the file
    writeFileSync('/tmp/test.txt', 'Hello World');
    ok(existsSync('/tmp/test.txt'));
    let stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 11);

    // Truncate to 10 bytes
    truncateSync('/tmp/test.txt', 10);
    stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 10);

    // Truncate to 20 bytes
    truncateSync('/tmp/test.txt', 20);
    stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 20);

    // Truncate to 0 bytes
    truncateSync('/tmp/test.txt', 0);
    stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 0);

    // Truncate to a negative size throws an error
    throws(() => truncateSync('/tmp/test.txt', -1), kErrOutOfRange);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      truncate('/tmp/test.txt', 5, (err) => {
        if (err) return reject(err);
        const stat2 = statSync('/tmp/test.txt');
        strictEqual(stat2.size, 5);
        resolve();
      });
      await promise;
    }

    throws(() => truncate('/tmp/test.txt', -1), kErrOutOfRange);
  },
};

export const writeSyncTest = {
  test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    throws(() => writeSync(''), kErrInvalidArgType);
    throws(() => writeSync(fd, 'Hello World', ''), kErrInvalidArgType);
    throws(
      () => writeSync(fd, 'Hello World', { position: 'hello' }),
      kErrInvalidArgType
    );
    throws(() => writeSync(123, 'Hello World'), kErrEBadf);
    throws(() => writeSync(fd, Buffer.alloc(2), { offset: 5 }), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });
    throws(() => writeSync(fd, Buffer.alloc(2), { length: 5 }), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });
    throws(
      () => writeSync(fd, Buffer.alloc(2), { offset: 'hello' }),
      kErrInvalidArgType
    );

    writeSync(fd, 'Hello World');
    writeSync(fd, '!!!!');
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const dest = Buffer.alloc(Number(stat2.size));

    // When we don't specify a position, it reads from the current position,
    // which currently is the end of the file... so we get nothing here.
    strictEqual(readSync(fd, dest), 0);

    // But when we do specify a position, we can read from the beginning...
    strictEqual(readSync(fd, dest, 0, dest.byteLength, 0), dest.byteLength);
    strictEqual(dest.toString(), 'Hello World!!!!');

    // Likewise, we can use an options object for the position
    dest.fill(0);
    strictEqual(dest.toString(), '\0'.repeat(dest.byteLength));
    strictEqual(readSync(fd, dest, { position: 0 }), 15);
    strictEqual(dest.toString(), 'Hello World!!!!');

    const dest2 = readFileSync('/tmp/test.txt');
    const dest3 = readFileSync(fd);
    const dest4 = readFileSync(fd, { encoding: 'utf8' });
    strictEqual(dest2.toString(), 'Hello World!!!!');
    strictEqual(dest3.toString(), 'Hello World!!!!');
    strictEqual(dest4, 'Hello World!!!!');

    closeSync(fd);
  },
};

export const writeAsyncCallbackTest = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    function mustNotCall() {
      throw new Error('This function must not be called');
    }

    throws(() => write(fd, 'Hello World'), kErrInvalidArgType);
    throws(() => write(fd, 'Hello World', '', mustNotCall), kErrInvalidArgType);
    throws(
      () => write(fd, 'Hello World', { position: 'hello' }, mustNotCall),
      kErrInvalidArgType
    );
    throws(() => write(fd, Buffer.alloc(2), { offset: 5 }, mustNotCall), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });
    throws(() => write(fd, Buffer.alloc(2), { length: 5 }, mustNotCall), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });
    throws(
      () => write(fd, Buffer.alloc(2), { offset: 'hello' }, mustNotCall),
      kErrInvalidArgType
    );

    await new Promise((resolve, reject) => {
      write(fd, 'Hello World', (err, written) => {
        if (err) return reject(err);
        strictEqual(written, 11);
        resolve();
      });
    });

    await new Promise((resolve, reject) => {
      write(fd, '!!!!', (err, written) => {
        if (err) return reject(err);
        strictEqual(written, 4);
        resolve();
      });
    });

    await rejects(
      new Promise((resolve, reject) => {
        write(123, 'Hello World', { position: 0 }, (err) => {
          if (err) return reject(err);
          resolve();
        });
      }),
      kErrEBadf
    );

    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const dest = Buffer.alloc(Number(stat2.size));

    // When we don't specify a position, it reads from the current position,
    // which currently is the end of the file... so we get nothing here.
    strictEqual(readSync(fd, dest), 0);

    // But when we do specify a position, we can read from the beginning...
    strictEqual(readSync(fd, dest, 0, dest.byteLength, 0), dest.byteLength);
    strictEqual(dest.toString(), 'Hello World!!!!');

    closeSync(fd);
  },
};

export const writeSyncTest2 = {
  test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    strictEqual(writeSync(fd, 'Hello World', 2n), 11);

    // Writing to a position beyond max uint32_t is not allowed.
    throws(() => writeSync(fd, 'Hello World', 2n ** 32n), {
      code: 'EINVAL',
    });
    throws(() => writeSync(fd, 'Hello World', 2 ** 32), {
      code: 'ERR_OUT_OF_RANGE',
    });

    strictEqual(writeSync(fd, 'aa', 0, 'ascii'), 2);

    const stat2 = fstatSync(fd);
    strictEqual(stat2.size, 13);

    const dest = Buffer.alloc(stat2.size);
    strictEqual(readSync(fd, dest, 0, dest.byteLength, 0), 13);
    strictEqual(dest.toString(), 'aaHello World');

    closeSync(fd);
  },
};

export const writeSyncTest3 = {
  test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    writeSync(fd, Buffer.from('Hello World'));
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 11n);

    closeSync(fd);
  },
};

export const writeSyncTest4 = {
  test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    // Writing a partial buffer works
    writeSync(fd, Buffer.from('Hello World'), 1, 3, 1);

    // Specifying an offset or length beyond the buffer size is not allowed.
    throws(() => writeSync(fd, Buffer.from('Hello World'), 100, 3), {
      message: /outside of buffer bounds/,
    });
    // Specifying an offset or length beyond the buffer size is not allowed.
    throws(() => writeSync(fd, Buffer.from('Hello World'), 0, 100), {
      message: /outside of buffer bounds/,
    });

    throws(() => writeSync(fd, Buffer.from('hello world'), 'a'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => writeSync(fd, Buffer.from('hello world'), 1n), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => writeSync(fd, Buffer.from('hello world'), 0, 'a'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => writeSync(fd, Buffer.from('hello world'), 1, 1n), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => writeSync(fd, 123), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 4n);

    closeSync(fd);
  },
};

export const writeReadOffsetBeyondLength = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    const heap = new Uint8Array(64);
    for (let i = 0; i < heap.length; i++) heap[i] = i;

    // offset > length is valid as long as offset + length <= byteLength
    strictEqual(writeSync(fd, heap, 10, 2), 2);
    strictEqual(writeSync(fd, heap, 20, 3, 2), 3);
    strictEqual(writeSync(fd, heap, { offset: 30, length: 1, position: 5 }), 1);
    strictEqual(writeSync(fd, heap, { offset: 62, position: 6 }), 2);
    strictEqual(writeSync(fd, heap, 63, undefined, 8), 1);
    strictEqual(writeSync(fd, heap, 64, 0, 9), 0);
    strictEqual(fstatSync(fd).size, 9);

    // A view into a larger backing buffer: offset is relative to the view.
    const view = new Uint8Array(heap.buffer, 8, 16);
    strictEqual(writeSync(fd, view, 12, 4, 9), 4);
    strictEqual(fstatSync(fd).size, 13);

    throws(() => writeSync(fd, heap, 65, 0), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });
    throws(() => writeSync(fd, heap, 60, 5), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });
    throws(() => writeSync(fd, view, 12, 5), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });
    throws(() => writeSync(fd, view, 17), {
      code: 'ERR_BUFFER_OUT_OF_BOUNDS',
    });

    const dest = new Uint8Array(64);
    strictEqual(readSync(fd, dest, 40, 13, 0), 13);
    deepStrictEqual(
      [...dest.subarray(40, 53)],
      [10, 11, 20, 21, 22, 30, 62, 63, 63, 20, 21, 22, 23]
    );
    strictEqual(readSync(fd, dest, 50, 2, 0), 2);
    strictEqual(readSync(fd, dest, { offset: 60, position: 0 }), 4);
    deepStrictEqual([...dest.subarray(60)], [10, 11, 20, 21]);
    strictEqual(readSync(fd, dest, 64, 0, 0), 0);

    const destView = new Uint8Array(dest.buffer, 8, 16);
    destView.fill(0);
    strictEqual(readSync(fd, destView, 12, 4, 0), 4);
    deepStrictEqual([...destView.subarray(12)], [10, 11, 20, 21]);
    deepStrictEqual([...dest.subarray(24, 26)], [0, 0]);

    // Zero-length reads are a no-op regardless of offset, as in Node.
    strictEqual(readSync(fd, dest, 65, 0, 0), 0);
    strictEqual(readSync(fd, destView, 17, 0, 0), 0);
    throws(() => readSync(fd, dest, 60, 5, 0), kErrOutOfRange);
    throws(() => readSync(fd, destView, 12, 5, 0), kErrOutOfRange);
    throws(() => readSync(fd, dest, 65, 1, 0), kErrOutOfRange);

    // Callback and promise variants share the same validation.
    await new Promise((resolve, reject) => {
      write(fd, heap, 10, 2, 13, (err, written) => {
        if (err) return reject(err);
        strictEqual(written, 2);
        resolve();
      });
    });
    await new Promise((resolve, reject) => {
      read(fd, dest, 40, 2, 13, (err, bytesRead) => {
        if (err) return reject(err);
        strictEqual(bytesRead, 2);
        deepStrictEqual([...dest.subarray(40, 42)], [10, 11]);
        resolve();
      });
    });
    await new Promise((resolve, reject) => {
      read(fd, dest, 65, 0, 0, (err, bytesRead) => {
        if (err) return reject(err);
        strictEqual(bytesRead, 0);
        resolve();
      });
    });
    closeSync(fd);

    const handle = await promises.open('/tmp/test.txt', 'r+');
    strictEqual((await handle.write(heap, 10, 2, 15)).bytesWritten, 2);
    strictEqual((await handle.read(dest, 40, 2, 15)).bytesRead, 2);
    strictEqual((await handle.read(dest, 62)).bytesRead, 2);
    strictEqual((await handle.read(destView, 12, 4, 15)).bytesRead, 2);
    strictEqual((await handle.read(dest, 65, 0, 0)).bytesRead, 0);
    await handle.close();
  },
};

export const writeSyncAppend = {
  test() {
    const fd = openSync('/tmp/test.txt', 'a');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    // In append mode, the position is ignored.

    writeSync(fd, 'Hello World', 1000);
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 11n);

    writeSync(fd, '!!!!', 2000);
    const stat3 = fstatSync(fd, { bigint: true });
    strictEqual(stat3.size, 15n);

    closeSync(fd);
  },
};

export const writevSyncTest = {
  test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    writevSync(fd, [Buffer.from('Hello World'), Buffer.from('!!!!')]);

    throws(() => writevSync(fd, [1, 2]), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    throws(() => writevSync(100, [Buffer.from('')]), {
      message: 'bad file descriptor',
      code: 'EBADF',
    });

    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const dest1 = Buffer.alloc(5);
    const dest2 = Buffer.alloc(10);
    const dest3 = Buffer.alloc(5);
    let read = readvSync(fd, [dest1, dest2, dest3], 0);
    strictEqual(read, 15);
    let dest = Buffer.concat([dest1, dest2, dest3]);
    strictEqual(dest.toString('utf8', 0, read), 'Hello World!!!!');

    dest1.fill(0);
    dest2.fill(0);
    dest3.fill(0);
    read = readvSync(fd, [dest1, dest2, dest3], 1);
    strictEqual(read, 14);
    dest = Buffer.concat([dest1, dest2, dest3]);
    strictEqual(dest.toString('utf8', 0, read), 'ello World!!!!');

    // Reading from a position beyond the end of the file returns nothing.
    strictEqual(readvSync(fd, [dest1], 100), 0);

    closeSync(fd);
  },
};

export const writevAsyncCallbackTest = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    throws(() => writev('hello'), kErrInvalidArgType);
    throws(() => writev(0, 123), kErrInvalidArgType);
    throws(() => writev(fd, [Buffer.from('')], 123), kErrInvalidArgType);

    await new Promise((resolve, reject) => {
      writev(
        fd,
        [Buffer.from('Hello World'), Buffer.from('!!!!')],
        (err, written) => {
          if (err) return reject(err);
          strictEqual(written, 15);
          resolve();
        }
      );
    });

    throws(
      () =>
        writev(fd, [1, 2], (err) => {
          if (err) throw err;
        }),
      {
        code: /ERR_INVALID_ARG_TYPE/,
      }
    );

    await rejects(
      new Promise((resolve, reject) => {
        writev(100, [Buffer.from('')], (err) => {
          if (err) return reject(err);
          resolve();
        });
      }),
      {
        code: 'EBADF',
      }
    );

    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const dest1 = Buffer.alloc(5);
    const dest2 = Buffer.alloc(10);
    const dest3 = Buffer.alloc(5);
    let read = readvSync(fd, [dest1, dest2, dest3], 0);
    strictEqual(read, 15);
    let dest = Buffer.concat([dest1, dest2, dest3]);
    strictEqual(dest.toString('utf8', 0, read), 'Hello World!!!!');

    dest1.fill(0);
    dest2.fill(0);
    dest3.fill(0);
    read = readvSync(fd, [dest1, dest2, dest3], 1);
    strictEqual(read, 14);
    dest = Buffer.concat([dest1, dest2, dest3]);
    strictEqual(dest.toString('utf8', 0, read), 'ello World!!!!');

    // Reading from a position beyond the end of the file returns nothing.
    strictEqual(readvSync(fd, [dest1], 100), 0);

    closeSync(fd);
  },
};

export const writeFileSyncTest = {
  test() {
    ok(!existsSync('/tmp/test.txt'));
    strictEqual(writeFileSync('/tmp/test.txt', 'Hello World'), 11);
    ok(existsSync('/tmp/test.txt'));
    let stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 11);
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World');

    strictEqual(appendFileSync('/tmp/test.txt', '!!!!'), 4);
    stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 15);
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World!!!!');

    // We can also use a file descriptor
    const fd = openSync('/tmp/test.txt', 'a+');
    writeFileSync(fd, '##');
    strictEqual(readFileSync(fd).toString(), 'Hello World!!!!##');
    closeSync(fd);
  },
};

export const appendFileSyncFlush = {
  test() {
    ok(!existsSync('/tmp/test.txt'));

    // The flush option really is not supported in any particular way in
    // our implementation but let's verify it.
    appendFileSync('/tmp/test.txt', 'hello world', { flush: true });

    ['no', {}, null, -1].forEach((i) => {
      throws(
        () => appendFileSync('/tmp/test.txt', 'hello world', { flush: 'no' }),
        {
          code: 'ERR_INVALID_ARG_TYPE',
        }
      );
    });

    ok(existsSync('/tmp/test.txt'));
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'hello world');
  },
};

export const writeFileAsyncCallbackTest = {
  async test() {
    ok(!existsSync('/tmp/test.txt'));

    throws(() => writeFile({}), kErrInvalidArgType);
    throws(() => writeFile('', 123), kErrInvalidArgType);
    throws(() => writeFile('', '', 123), kErrInvalidArgType);
    throws(() => writeFile('', '', {}), kErrInvalidArgType);

    await new Promise((resolve, reject) => {
      writeFile('/tmp/test.txt', 'Hello World', (err) => {
        if (err) return reject(err);
        ok(existsSync('/tmp/test.txt'));
        resolve();
      });
    });

    let stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 11);
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World');

    await new Promise((resolve, reject) => {
      appendFile('/tmp/test.txt', '!!!!', (err) => {
        strictEqual(
          readFileSync('/tmp/test.txt').toString(),
          'Hello World!!!!'
        );
        if (err) return reject(err);
        resolve();
      });
    });

    stat = statSync('/tmp/test.txt');
    strictEqual(stat.size, 15);
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World!!!!');

    // We can also use a file descriptor
    const fd = openSync('/tmp/test.txt', 'a+');
    await new Promise((resolve, reject) => {
      writeFile(fd, '##', 'utf8', (err) => {
        if (err) return reject(err);
        resolve();
      });
    });
    strictEqual(readFileSync(fd).toString(), 'Hello World!!!!##');
    closeSync(fd);

    // We can use the promise API as well.
    await promises.appendFile('/tmp/test.txt', '!!!');
    strictEqual(
      readFileSync('/tmp/test.txt').toString(),
      'Hello World!!!!##!!!'
    );
  },
};

export const appendFileCases = {
  async test() {
    ok(!existsSync('/tmp/test.txt'));
    // It accepts bufers
    appendFileSync('/tmp/test.txt', Buffer.from('Hello World'));
    ok(existsSync('/tmp/test.txt'));
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World');

    // With the callback API also
    const { promise, resolve, reject } = Promise.withResolvers();
    appendFile('/tmp/test.txt', Buffer.from('!!!!'), (err) => {
      if (err) return reject(err);
      strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World!!!!');
      resolve();
    });
    await promise;

    // And the promises API
    await promises.appendFile('/tmp/test.txt', Buffer.from('!!!'));
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World!!!!!!!');

    // But invalid types throw errors
    [123, {}, null, []].forEach((data) => {
      throws(() => appendFileSync('/tmp/test.txt', data), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
    });
  },
};

export const readAsyncCallbackTest = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    throws(() => read('hello'), kErrInvalidArgType);
    throws(() => read(0, 123), kErrInvalidArgType);
    throws(() => read(0, Buffer.alloc(1), 123), kErrInvalidArgType);
    throws(() => read(0, Buffer.alloc(1), {}), kErrInvalidArgType);

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    writeSync(fd, 'Hello World');
    writeSync(fd, '!!!!');
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const dest = Buffer.alloc(15);
    let bytesRead = await new Promise((resolve, reject) => {
      read(fd, dest, 0, dest.byteLength, 0, (err, bytesRead) => {
        if (err) return reject(err);
        resolve(bytesRead);
      });
    });

    strictEqual(bytesRead, 15);
    strictEqual(dest.toString(), 'Hello World!!!!');

    closeSync(fd);
  },
};

export const readvAsyncCallbackTest = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    throws(() => readv('hello'), kErrInvalidArgType);
    throws(() => readv(0, 123), kErrInvalidArgType);
    throws(() => readv(0, [Buffer.alloc(1)], 123), kErrInvalidArgType);
    throws(() => readv(0, [Buffer.alloc(1)], {}), kErrInvalidArgType);

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    writeSync(fd, 'Hello World');
    writeSync(fd, '!!!!');
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const dest1 = Buffer.alloc(5);
    const dest2 = Buffer.alloc(10);
    let bytesRead = await new Promise((resolve, reject) => {
      readv(fd, [dest1, dest2], 0, (err, bytesRead) => {
        if (err) return reject(err);
        resolve(bytesRead);
      });
    });

    strictEqual(bytesRead, 15);
    strictEqual(Buffer.concat([dest1, dest2]).toString(), 'Hello World!!!!');

    closeSync(fd);
  },
};

export const readFileAsyncCallbackTest = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    throws(() => readFile('hello'), kErrInvalidArgType);
    throws(() => readFile(0, 123), kErrInvalidArgType);
    throws(() => readFile(0, {}), kErrInvalidArgType);

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    writeSync(fd, 'Hello World');
    writeSync(fd, '!!!!');
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const content = await new Promise((resolve, reject) => {
      readFile(fd, (err, content) => {
        if (err) return reject(err);
        resolve(content);
      });
    });

    strictEqual(content.toString(), 'Hello World!!!!');

    closeSync(fd);
  },
};

export const copyAndRenameSyncTest = {
  test() {
    ok(!existsSync('/tmp/test.txt'));
    ok(!existsSync('/tmp/test2.txt'));
    writeFileSync('/tmp/test.txt', 'Hello World');
    ok(existsSync('/tmp/test.txt'));
    ok(!existsSync('/tmp/test2.txt'));

    copyFileSync('/tmp/test.txt', '/tmp/test2.txt');
    // Both files exist
    ok(existsSync('/tmp/test.txt'));
    ok(existsSync('/tmp/test2.txt'));

    strictEqual(
      readFileSync('/tmp/test.txt').toString(),
      readFileSync('/tmp/test2.txt').toString()
    );

    copyFileSync('/tmp/test.txt', '/tmp/test4.txt', 0);
    // Both files exist
    ok(existsSync('/tmp/test.txt'));
    ok(existsSync('/tmp/test4.txt'));

    strictEqual(
      readFileSync('/tmp/test.txt').toString(),
      readFileSync('/tmp/test4.txt').toString()
    );

    throws(
      () => copyFileSync('/tmp/test.txt', '/tmp/test4.txt', COPYFILE_EXCL),
      {
        code: 'EEXIST',
      }
    );

    throws(
      () =>
        copyFileSync(
          '/tmp/test.txt',
          '/tmp/nope.txt',
          constants.COPYFILE_FICLONE_FORCE
        ),
      {
        message: /unsupported/,
      }
    );

    copyFileSync('/tmp/test.txt', '/tmp/test5.txt', constants.COPYFILE_FICLONE);
    // Both files exist
    ok(existsSync('/tmp/test.txt'));
    ok(existsSync('/tmp/test5.txt'));

    strictEqual(
      readFileSync('/tmp/test.txt').toString(),
      readFileSync('/tmp/test5.txt').toString()
    );

    [false, 1, {}, [], null, undefined].forEach((i) => {
      throws(() => copyFileSync(i, '/tmp/nope.txt'), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
      throws(() => copyFileSync('/tmp/test.txt', i), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
    });
    [false, {}, [], null].forEach((i) => {
      throws(() => copyFileSync('/tmp/test.txt', '/tmp/test2.txt', i), {
        code: 'ERR_INVALID_ARG_VALUE',
      });
    });

    // We can modify one of the files and the other remains unchanged
    writeFileSync('/tmp/test.txt', 'Hello World 2');
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World 2');
    strictEqual(readFileSync('/tmp/test2.txt').toString(), 'Hello World');

    // Renaming the files work
    renameSync('/tmp/test.txt', '/tmp/test3.txt');
    ok(!existsSync('/tmp/test.txt'));
    ok(existsSync('/tmp/test3.txt'));
    strictEqual(readFileSync('/tmp/test3.txt').toString(), 'Hello World 2');
  },
};

export const copyAndRenameAsyncCallbackTest = {
  async test() {
    ok(!existsSync('/tmp/test.txt'));
    ok(!existsSync('/tmp/test2.txt'));
    writeFileSync('/tmp/test.txt', 'Hello World');
    ok(existsSync('/tmp/test.txt'));
    ok(!existsSync('/tmp/test2.txt'));

    throws(() => copyFile(123), kErrInvalidArgType);
    throws(() => copyFile('/tmp/test.txt', 123), kErrInvalidArgType);
    throws(
      () => copyFile('/tmp/test.txt', '/tmp/test2.txt', 123),
      kErrInvalidArgValue
    );
    throws(
      () => copyFile('/tmp/test.txt', '/tmp/test2.txt', 0),
      kErrInvalidArgType
    );

    throws(() => rename(123), kErrInvalidArgType);
    throws(() => rename('/tmp/test.txt', 123), kErrInvalidArgType);
    throws(
      () => rename('/tmp/test.txt', '/tmp/test2.txt', 0),
      kErrInvalidArgType
    );

    await new Promise((resolve, reject) => {
      copyFile('/tmp/test.txt', '/tmp/test2.txt', (err) => {
        if (err) return reject(err);
        resolve();
      });
    });

    // Test the exclusive option fails when the file already exists
    await rejects(
      new Promise((resolve, reject) => {
        copyFile('/tmp/test.txt', '/tmp/test2.txt', COPYFILE_EXCL, (err) => {
          if (err) return reject(err);
          resolve();
        });
      }),
      kErrEExist
    );

    // Both files exist
    ok(existsSync('/tmp/test.txt'));
    ok(existsSync('/tmp/test2.txt'));

    strictEqual(
      readFileSync('/tmp/test.txt').toString(),
      readFileSync('/tmp/test2.txt').toString()
    );

    // We can modify one of the files and the other remains unchanged
    writeFileSync('/tmp/test.txt', 'Hello World 2');
    strictEqual(readFileSync('/tmp/test.txt').toString(), 'Hello World 2');
    strictEqual(readFileSync('/tmp/test2.txt').toString(), 'Hello World');

    // Renaming the files work
    await new Promise((resolve, reject) => {
      rename('/tmp/test.txt', '/tmp/test3.txt', (err) => {
        if (err) return reject(err);
        resolve();
      });
    });

    ok(!existsSync('/tmp/test.txt'));
    ok(existsSync('/tmp/test3.txt'));
    strictEqual(readFileSync('/tmp/test3.txt').toString(), 'Hello World 2');
  },
};

export const fsCwdTest = {
  test() {
    process.chdir('/bundle');

    throws(
      () => {
        writeFileSync('test-cwd.txt', 'Hello from original cwd');
      },
      { code: 'EPERM' }
    );

    process.chdir('/tmp');

    writeFileSync('test-cwd.txt', 'Hello from /tmp');
    ok(existsSync('test-cwd.txt'));
    ok(existsSync('/tmp/test-cwd.txt'));

    ok(existsSync('test-cwd.txt'));
    ok(!existsSync(`/bundle/test-cwd.txt`));

    strictEqual(readFileSync('test-cwd.txt').toString(), 'Hello from /tmp');
    strictEqual(
      readFileSync('/tmp/test-cwd.txt').toString(),
      'Hello from /tmp'
    );

    process.chdir('/bundle');

    ok(!existsSync('test-cwd.txt'));
    throws(
      () => {
        readFileSync('test-cwd.txt');
      },
      { code: 'ENOENT', path: '/bundle/test-cwd.txt' }
    );

    unlinkSync('/tmp/test-cwd.txt');
  },
};

export const readBadEncoding = {
  test() {
    const kErrorObj = {
      code: 'ERR_INVALID_ARG_VALUE',
    };
    throws(() => readFileSync('/tmp/test.txt', 'bad-encoding'), kErrorObj);
    throws(
      () => appendFileSync('/tmp/test.txt', 'data', 'bad-encoding'),
      kErrorObj
    );
    throws(() => readdirSync('/tmp/test.txt', 'bad-encoding'), kErrorObj);
    throws(() => readlinkSync('/tmp/test.txt', 'bad-encoding'), kErrorObj);
    throws(
      () => writeFileSync('/tmp/test.txt', 'data', 'bad-encoding'),
      kErrorObj
    );
    throws(
      () => appendFileSync('/tmp/test.txt', 'data', 'bad-encoding'),
      kErrorObj
    );
    throws(() => realpathSync('/tmp/test.txt', 'bad-encoding'), kErrorObj);
    throws(() => mkdtempSync('/tmp/test.txt', 'bad-encoding'), kErrorObj);
    throws(() => ReadStream('/tmp/test.txt', 'bad-encoding'), kErrorObj);
    throws(() => WriteStream('/tmp/test.txt', 'bad-encoding'), kErrorObj);

    throws(
      () => writeFile('/tmp/test.txt', 'data', 'bad-encoding', mustNotCall),
      kErrorObj
    );
    throws(
      () => appendFile('/tmp/test.txt', 'data', 'bad-encoding', mustNotCall),
      kErrorObj
    );

    function mustNotCall() {
      throw new Error('This function must not be called');
    }

    throws(
      () => readFile('/tmp/test.txt', 'bad-encoding', mustNotCall),
      kErrorObj
    );
    throws(
      () => readdir('/tmp/test.txt', 'bad-encoding', mustNotCall),
      kErrorObj
    );
    throws(
      () => readlink('/tmp/test.txt', 'bad-encoding', mustNotCall),
      kErrorObj
    );
    throws(
      () => realpath('/tmp/test.txt', 'bad-encoding', mustNotCall),
      kErrorObj
    );
    throws(
      () => mkdtemp('/tmp/test.txt', 'bad-encoding', mustNotCall),
      kErrorObj
    );
  },
};

export const fsConstantsTest = {
  test() {
    // Check if the two constants accepted by chmod() on Windows are defined.
    notStrictEqual(constants.S_IRUSR, undefined);
    notStrictEqual(constants.S_IWUSR, undefined);

    // Check null prototype.
    strictEqual(Object.getPrototypeOf(constants), null);

    const knownFsConstantNames = [
      'UV_FS_SYMLINK_DIR',
      'UV_FS_SYMLINK_JUNCTION',
      'O_RDONLY',
      'O_WRONLY',
      'O_RDWR',
      'UV_DIRENT_UNKNOWN',
      'UV_DIRENT_FILE',
      'UV_DIRENT_DIR',
      'UV_DIRENT_LINK',
      'UV_DIRENT_FIFO',
      'UV_DIRENT_SOCKET',
      'UV_DIRENT_CHAR',
      'UV_DIRENT_BLOCK',
      'S_IFMT',
      'S_IFREG',
      'S_IFDIR',
      'S_IFCHR',
      'S_IFBLK',
      'S_IFIFO',
      'S_IFLNK',
      'S_IFSOCK',
      'O_CREAT',
      'O_EXCL',
      'UV_FS_O_FILEMAP',
      'O_NOCTTY',
      'O_TRUNC',
      'O_APPEND',
      'O_DIRECTORY',
      'O_EXCL',
      'O_NOATIME',
      'O_NOFOLLOW',
      'O_SYNC',
      'O_DSYNC',
      'O_SYMLINK',
      'O_DIRECT',
      'O_NONBLOCK',
      'S_IRWXU',
      'S_IRUSR',
      'S_IWUSR',
      'S_IXUSR',
      'S_IRWXG',
      'S_IRGRP',
      'S_IWGRP',
      'S_IXGRP',
      'S_IRWXO',
      'S_IROTH',
      'S_IWOTH',
      'S_IXOTH',
      'F_OK',
      'R_OK',
      'W_OK',
      'X_OK',
      'UV_FS_COPYFILE_EXCL',
      'COPYFILE_EXCL',
      'UV_FS_COPYFILE_FICLONE',
      'COPYFILE_FICLONE',
      'UV_FS_COPYFILE_FICLONE_FORCE',
      'COPYFILE_FICLONE_FORCE',
      'EXTENSIONLESS_FORMAT_JAVASCRIPT',
      'EXTENSIONLESS_FORMAT_WASM',
    ];

    const fsConstantNames = Object.keys(constants);
    const unknownFsConstantNames = fsConstantNames.filter((constant) => {
      return !knownFsConstantNames.includes(constant);
    });
    deepStrictEqual(
      unknownFsConstantNames,
      [],
      `Unknown fs.constants: ${unknownFsConstantNames.join(', ')}`
    );

    strictEqual(typeof constants.COPYFILE_EXCL, 'number');
    strictEqual(typeof constants.COPYFILE_FICLONE, 'number');
    strictEqual(typeof constants.COPYFILE_FICLONE_FORCE, 'number');
    strictEqual(typeof constants.UV_FS_COPYFILE_EXCL, 'number');
    strictEqual(typeof constants.UV_FS_COPYFILE_FICLONE, 'number');
    strictEqual(typeof constants.UV_FS_COPYFILE_FICLONE_FORCE, 'number');
    strictEqual(constants.COPYFILE_EXCL, constants.UV_FS_COPYFILE_EXCL);
    strictEqual(constants.COPYFILE_FICLONE, constants.UV_FS_COPYFILE_FICLONE);
    strictEqual(
      constants.COPYFILE_FICLONE_FORCE,
      constants.UV_FS_COPYFILE_FICLONE_FORCE
    );
  },
};

export const fmapIgnoredTest = {
  test() {
    const filename = '/tmp/foo';
    const text = 'Memory File Mapping Test';

    const mw =
      constants.UV_FS_O_FILEMAP |
      constants.O_TRUNC |
      constants.O_CREAT |
      constants.O_WRONLY;
    const mr = constants.UV_FS_O_FILEMAP | constants.O_RDONLY;

    writeFileSync(filename, text, { flag: mw });
    const r1 = readFileSync(filename, { encoding: 'utf8', flag: mr });
    strictEqual(r1, text);
  },
};

export const fileNamesWithNullBytesTest = {
  test() {
    const filename = '/tmp/test\0.txt';
    throws(() => writeFileSync(filename, 'Hello World'), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
  },
};

export const fileNamesWithSurrogatePairsTest = {
  test() {
    const tempdir = mkdtempSync('/tmp/emoji-fruit-🍇 🍈 🍉 🍊 🍋');
    ok(existsSync(tempdir));
    const filename = '🚀🔥🛸.txt';
    const content = 'Test content';
    writeFileSync(join(tempdir, filename), content);
    const readContent = readFileSync(join(tempdir, filename), 'utf8');
    strictEqual(readContent, content);
  },
};

export const testFsWithZlib = {
  async test() {
    const imgBase64 =
      'iVBORw0KGgoAAAANSUhEUgAAAAgAAAAIAQMAAAD+wSzIAAAABlBMVEX///+/v7+jQ3Y5AAAADklEQVQI12P4AIX8EAgALgAD/aNpbtEAAAAASUVORK5CYII';

    const imageStream = Readable.from(Buffer.from(imgBase64, 'base64'));
    await new Promise((resolve, reject) => {
      const outStream = createWriteStream(`/tmp/image-stream.png.gz`);
      outStream.on('close', resolve).on('error', reject);
      imageStream.pipe(zlib.createGzip()).pipe(outStream);
    });

    doesNotThrow(() => {
      zlib
        .gunzipSync(readFileSync('/tmp/image-stream.png.gz'))
        .toString('base64');
    });
  },
};
