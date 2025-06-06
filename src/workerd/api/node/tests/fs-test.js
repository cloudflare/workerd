// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ifError, ok, strictEqual, throws } from 'node:assert';

import {
  existsSync,
  statSync,
  openSync,
  closeSync,
  fstatSync,
  ftruncateSync,
  fsyncSync,
  fdatasyncSync,
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
  fsync,
  fdatasync,
} from 'node:fs';

export const openCloseTest = {
  async test() {
    ok(!existsSync('/tmp/test.txt'));
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    ok(stat);
    ok(stat.isFile());
    ok(!stat.isDirectory());
    strictEqual(stat.size, 0n);

    throws(() => fstatSync(123), {
      message: /bad file descriptor/,
      code: 'EBADF',
    });
    throws(() => fstatSync(fd, { bigint: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => fstatSync('abc'), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    strictEqual(fd, 0);

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

    ftruncateSync(fd, 10);
    const stat2 = fstatSync(fd);
    ok(stat2);
    ok(stat2.isFile());
    strictEqual(stat2.size, 10);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      ftruncate(fd, 20, (err) => {
        if (err) return reject(err);
        const stat3 = fstatSync(fd);
        ok(stat3);
        ok(stat3.isFile());
        strictEqual(stat3.size, 20);
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fsync(fd, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fdatasync(fd, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    fsyncSync(fd);
    fdatasyncSync(fd);

    // Close the file
    closeSync(fd);
    // Can close multiple times
    closeSync(fd);
    // Can close non-existent file descriptors
    closeSync(123);

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

export const writeSyncTest = {
  test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    writeSync(fd, 'Hello World');
    writeSync(fd, '!!!!');
    const stat2 = fstatSync(fd, { bigint: true });
    strictEqual(stat2.size, 15n);

    const dest = Buffer.alloc(15);

    // When we don't specify a position, it reads from the current position,
    // which currently is the end of the file... so we get nothint here.
    strictEqual(readSync(fd, dest), 0);

    // But when we do specify a position, we can read from the beginning...
    strictEqual(readSync(fd, dest, 0, dest.byteLength, 0), 15);
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

export const writeSyncTest2 = {
  test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.size, 0n);

    strictEqual(writeSync(fd, 'Hello World', 2n), 11);

    // Writing to a position beyond max uint32_t is not allowed.
    throws(() => writeSync(fd, 'Hello World', 2n ** 32n), {
      message: 'Position out of range',
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
      message: /out of bounds/,
    });
    // Specifying an offset or length beyond the buffer size is not allowed.
    throws(() => writeSync(fd, Buffer.from('Hello World'), 0, 100), {
      message: /out of bounds/,
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

export const copyAndRenameTest = {
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
