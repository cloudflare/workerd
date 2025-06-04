// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  deepStrictEqual,
  notDeepStrictEqual,
  ifError,
  ok,
  strictEqual,
  throws,
} from 'node:assert';

import {
  existsSync,
  statSync,
  statfsSync,
  linkSync,
  lstatSync,
  symlinkSync,
  readlinkSync,
  realpathSync,
  unlinkSync,
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
  mkdirSync,
  mkdtempSync,
  rmSync,
  rmdirSync,
  readdirSync,
  stat,
  statfs,
  link,
  symlink,
  readlink,
  realpath,
  unlink,
  close,
  fstat,
  ftruncate,
  fsync,
  fdatasync,
} from 'node:fs';

export const statSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => statSync(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => statSync('/', { bigint: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => statSync('/', { throwIfNoEntry: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    // Known accessible paths
    const stat = statSync('/');
    ok(stat);
    ok(stat.isDirectory());
    ok(!stat.isFile());
    ok(!stat.isBlockDevice());
    ok(!stat.isCharacterDevice());
    ok(!stat.isFIFO());
    ok(!stat.isSocket());
    ok(!stat.isSymbolicLink());

    ok(statSync(Buffer.from('/')));
    ok(statSync(Buffer.from('/')).isDirectory());

    const bundleStat = statSync('/bundle');
    ok(bundleStat);
    ok(bundleStat.isDirectory());
    ok(!bundleStat.isFile());
    ok(!bundleStat.isBlockDevice());
    ok(!bundleStat.isCharacterDevice());
    ok(!bundleStat.isFIFO());
    ok(!bundleStat.isSocket());
    ok(!bundleStat.isSymbolicLink());

    const workerStat = statSync('/bundle/worker');
    ok(workerStat);
    ok(!workerStat.isDirectory());
    ok(workerStat.isFile());
    ok(!workerStat.isBlockDevice());
    ok(!workerStat.isCharacterDevice());
    ok(!workerStat.isFIFO());
    ok(!workerStat.isSocket());
    ok(!workerStat.isSymbolicLink());

    const devStat = statSync('/dev/null');
    ok(devStat);
    ok(!devStat.isDirectory());
    ok(!devStat.isFile());
    ok(!devStat.isBlockDevice());
    ok(devStat.isCharacterDevice());
    ok(!devStat.isFIFO());
    ok(!devStat.isSocket());
    ok(!devStat.isSymbolicLink());

    strictEqual(bundleStat.dev, 0);
    strictEqual(devStat.dev, 1);

    const devStatBigInt = statSync('/dev/null', { bigint: true });
    ok(devStatBigInt);
    strictEqual(typeof devStatBigInt.dev, 'bigint');
    strictEqual(devStatBigInt.dev, 1n);

    // Paths that don't exist throw by default
    throws(() => statSync('/does/not/exist'), {
      code: 'ENOENT',
    });

    // Unless the `throwIfNoEntry` option is set to false, in which case it
    // returns undefined.
    const statNoThrow = statSync('/does/not/exist', { throwIfNoEntry: false });
    strictEqual(statNoThrow, undefined);
  },
};

export const statTest = {
  async test() {
    // Incorrect input types should throw.
    throws(() => stat(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => stat('/', { bigint: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => stat('/', { throwIfNoEntry: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    // Known accessible paths
    async function callStatSuccess(path, fn, bigint = false) {
      const { promise, resolve, reject } = Promise.withResolvers();
      stat(path, { bigint }, (err, stat) => {
        if (err) return reject(err);
        try {
          fn(stat);
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    async function callStatFail(path, code) {
      const { promise, resolve, reject } = Promise.withResolvers();
      stat(path, (err) => {
        if (err) {
          strictEqual(err.code, code);
          return resolve();
        }
        reject(new Error('Expected error was not thrown'));
      });
      await promise;
    }

    await callStatSuccess('/', (stat) => {
      ok(stat);
      ok(stat.isDirectory());
      ok(!stat.isFile());
      ok(!stat.isBlockDevice());
      ok(!stat.isCharacterDevice());
      ok(!stat.isFIFO());
      ok(!stat.isSocket());
      ok(!stat.isSymbolicLink());

      strictEqual(stat.dev, 0);
      strictEqual(typeof stat.dev, 'number');
    });

    await callStatSuccess(
      '/',
      (stat) => {
        ok(stat);
        ok(stat.isDirectory());
        ok(!stat.isFile());
        ok(!stat.isBlockDevice());
        ok(!stat.isCharacterDevice());
        ok(!stat.isFIFO());
        ok(!stat.isSocket());
        ok(!stat.isSymbolicLink());

        strictEqual(stat.dev, 0n);
        strictEqual(typeof stat.dev, 'bigint');
      },
      true /* bigint */
    );

    await callStatSuccess(Buffer.from('/'), (stat) => {
      ok(stat);
      ok(stat.isDirectory());
      ok(!stat.isFile());
      ok(!stat.isBlockDevice());
      ok(!stat.isCharacterDevice());
      ok(!stat.isFIFO());
      ok(!stat.isSocket());
      ok(!stat.isSymbolicLink());
    });

    await callStatSuccess(new URL('file:///'), (stat) => {
      ok(stat);
      ok(stat.isDirectory());
      ok(!stat.isFile());
      ok(!stat.isBlockDevice());
      ok(!stat.isCharacterDevice());
      ok(!stat.isFIFO());
      ok(!stat.isSocket());
      ok(!stat.isSymbolicLink());
    });

    await callStatFail('/does/not/exist', 'ENOENT');
  },
};

export const statfsTest = {
  async test() {
    const check = {
      type: 0,
      bsize: 0,
      blocks: 0,
      bfree: 0,
      bavail: 0,
      files: 0,
      ffree: 0,
    };
    const checkBn = {
      type: 0n,
      bsize: 0n,
      blocks: 0n,
      bfree: 0n,
      bavail: 0n,
      files: 0n,
      ffree: 0n,
    };

    deepStrictEqual(statfsSync('/'), check);
    deepStrictEqual(statfsSync('/bundle', { bigint: true }), checkBn);

    async function callStatfs(path, fn, bigint = false) {
      const { promise, resolve, reject } = Promise.withResolvers();
      statfs(path, { bigint }, (err, stat) => {
        if (err) return reject(err);
        try {
          fn(stat);
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    await callStatfs('/', (stat) => {
      deepStrictEqual(stat, check);
    });
    await callStatfs(
      '/',
      (stat) => {
        deepStrictEqual(stat, checkBn);
      },
      true /* bigint */
    );

    // Throws if the path is invalid / wrong type
    throws(() => statfs(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    throws(() => statfs('/', { bigint: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
  },
};

export const linkTest = {
  async test() {
    ok(!existsSync('/tmp/a'));
    linkSync('/dev/null', '/tmp/a');
    ok(existsSync('/tmp/a'));

    // These are the same file.
    deepStrictEqual(statSync('/tmp/a'), statSync('/dev/null'));
    deepStrictEqual(lstatSync('/tmp/a'), statSync('/dev/null'));

    // Because this is a hard link, the realpath is /tmp/a
    strictEqual(realpathSync('/tmp/a'), '/tmp/a');

    // And readlinkSync throws
    throws(() => readlinkSync('/tmp/a'), {
      message: 'invalid argument',
    });

    ok(!existsSync('/tmp/b'));
    symlinkSync('/dev/null', '/tmp/b');
    ok(existsSync('/tmp/b'));

    deepStrictEqual(statSync('/tmp/b'), statSync('/dev/null'));
    const lstatB = lstatSync('/tmp/b');
    notDeepStrictEqual(lstatB, statSync('/dev/null'));
    ok(lstatB.isSymbolicLink());

    strictEqual(realpathSync('/tmp/b'), '/dev/null');
    strictEqual(readlinkSync('/tmp/b'), '/dev/null');

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      link('/dev/null', '/tmp/c', (err) => {
        try {
          ifError(err);
          ok(existsSync('/tmp/c'));
          deepStrictEqual(statSync('/tmp/c'), statSync('/dev/null'));
          deepStrictEqual(lstatSync('/tmp/c'), statSync('/dev/null'));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      symlink('/dev/null', '/tmp/d', (err) => {
        try {
          ifError(err);
          ok(existsSync('/tmp/d'));
          deepStrictEqual(statSync('/tmp/d'), statSync('/dev/null'));
          notDeepStrictEqual(lstatSync('/tmp/d'), statSync('/dev/null'));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      readlink('/tmp/d', (err, link) => {
        try {
          ifError(err);
          strictEqual(link, '/dev/null');
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      realpath('/tmp/d', (err, link) => {
        try {
          ifError(err);
          strictEqual(link, '/dev/null');
        } catch (err) {
          reject(err);
        }
        resolve();
      });
    }

    // Creating a symlink to a file that doesn't exist works.
    symlinkSync('/does/not/exist', '/tmp/e');

    // But creating a hard link to a file that doesn't exist throws.
    throws(() => linkSync('/does/not/exist', '/tmp/f'), {
      message: /file not found/,
    });

    // If the link name is empty, throw
    throws(() => symlinkSync('/does/not/exist', new URL('file:///tmp/g/')), {
      message: /Invalid filename/,
    });

    // If the directory does not exist, throw
    throws(() => symlinkSync('/does/not/exist', new URL('file:///tmp/a/b/c')), {
      message: /Directory does not exist/,
    });

    // If the file already exists, throw
    throws(() => symlinkSync('/does/not/exist', '/tmp/a'), {
      message: /File already exists/,
    });
    throws(() => linkSync('/does/not/exist', '/tmp/a'), {
      message: /File already exists/,
    });

    // If the destination is read-only, throw
    throws(() => symlinkSync('/dev/null', '/bundle/a'), {
      message: /Cannot add a file/,
    });
    throws(() => linkSync('/dev/null', '/bundle/a'), {
      message: /Cannot add a file/,
    });

    // unlinkSync removes things
    unlinkSync('/tmp/a');
    ok(!existsSync('/tmp/a'));

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      unlink('/tmp/b', (err) => {
        if (err) return reject(err);
        ok(!existsSync('/tmp/b'));
        resolve();
      });
      await promise;
    }

    // Cannot unlink read-only files
    throws(() => unlinkSync('/bundle/worker'), {
      message: /Cannot remove a file/,
    });

    // Cannot unlink directories
    throws(() => unlinkSync('/bundle'), {
      message: /Cannot unlink a directory/,
    });
  },
};

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

export const mkdirTest = {
  test() {
    ok(!existsSync('/tmp/testdir'));
    strictEqual(mkdirSync('/tmp/testdir'), undefined);
    ok(existsSync('/tmp/testdir'));

    ok(!existsSync('/tmp/testdir/a/b/c'));
    strictEqual(
      mkdirSync('/tmp/testdir/a/b/c', { recursive: true }),
      '/tmp/testdir/a'
    );
    ok(existsSync('/tmp/testdir/a/b/c'));

    // Cannot create a directory in a read-only location
    throws(() => mkdirSync('/bundle/a'), {
      message: /access is denied/,
    });

    // Creating a directory that already exists is a non-op
    mkdirSync('/tmp/testdir');

    // Attempting to create a directory that already exists as a file throws
    writeFileSync('/tmp/abc', 'Hello World');
    throws(() => mkdirSync('/tmp/abc'), {
      message: /File already exists/,
    });

    // Attempting to create a directory recursively when a parent is a file
    // throws
    throws(() => mkdirSync('/tmp/abc/foo', { recursive: true }), {
      message: /Invalid argument/,
    });

    // Passing incorrect types for options throws
    throws(() => mkdirSync('/tmp/testdir', { recursive: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    throws(() => mkdirSync('/tmp/testdir', 'abc'), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    strictEqual(mkdtempSync('/tmp/testdir-'), '/tmp/testdir-0');
    strictEqual(mkdtempSync('/tmp/testdir-'), '/tmp/testdir-1');
    throws(() => mkdtempSync('/bundle/testdir-'), {
      message: /access is denied/,
    });
  },
};

export const rmTest = {
  test() {
    ok(!existsSync('/tmp/testdir'));
    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');
    throws(() => rmdirSync('/tmp/testdir'), {
      message: /Directory is not empty/,
    });
    rmdirSync('/tmp/testdir', { recursive: true });

    ok(!existsSync('/tmp/testdir'));
    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');
    writeFileSync('/tmp/testdir/b.txt', 'Hello World');
    ok(existsSync('/tmp/testdir/a.txt'));

    // removing a file works
    rmSync('/tmp/testdir/a.txt');

    ok(!existsSync('/tmp/testdir/a.txt'));
    throws(() => rmSync('/tmp/testdir'));
    rmSync('/tmp/testdir', { recursive: true });

    // Passing incorrect types for options throws
    throws(() => rmSync('/tmp/testdir', { recursive: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmSync('/tmp/testdir', 'abc'), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmSync('/tmp/testdir', { force: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmSync('/tmp/testdir', { maxRetries: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmSync('/tmp/testdir', { retryDelay: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmSync('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmSync('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(
      () =>
        rmSync('/tmp/testdir', { maxRetries: 1, retryDelay: 1, force: 'yes' }),
      {
        code: /ERR_INVALID_ARG_TYPE/,
      }
    );

    throws(() => rmdirSync('/tmp/testdir', { recursive: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmdirSync('/tmp/testdir', 'abc'), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmdirSync('/tmp/testdir', { maxRetries: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => rmdirSync('/tmp/testdir', { retryDelay: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(
      () => rmdirSync('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }),
      {
        code: /ERR_INVALID_ARG_TYPE/,
      }
    );
    throws(
      () => rmdirSync('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }),
      {
        code: /ERR_INVALID_ARG_TYPE/,
      }
    );
  },
};

export const readdirTest = {
  test() {
    deepStrictEqual(readdirSync('/'), ['bundle', 'tmp', 'dev']);

    {
      const ents = readdirSync('/', { withFileTypes: true });
      strictEqual(ents.length, 3);

      strictEqual(ents[0].name, 'bundle');
      strictEqual(ents[0].isDirectory(), true);
      strictEqual(ents[0].isFile(), false);
      strictEqual(ents[0].isBlockDevice(), false);
      strictEqual(ents[0].isCharacterDevice(), false);
      strictEqual(ents[0].isFIFO(), false);
      strictEqual(ents[0].isSocket(), false);
      strictEqual(ents[0].isSymbolicLink(), false);
      strictEqual(ents[0].parentPath, '/');
    }

    {
      const ents = readdirSync('/', { withFileTypes: true, recursive: true });
      strictEqual(ents.length, 8);

      strictEqual(ents[0].name, 'bundle');
      strictEqual(ents[0].isDirectory(), true);
      strictEqual(ents[0].isFile(), false);
      strictEqual(ents[0].isBlockDevice(), false);
      strictEqual(ents[0].isCharacterDevice(), false);
      strictEqual(ents[0].isFIFO(), false);
      strictEqual(ents[0].isSocket(), false);
      strictEqual(ents[0].isSymbolicLink(), false);
      strictEqual(ents[0].parentPath, '/');

      strictEqual(ents[1].name, 'bundle/worker');
      strictEqual(ents[1].isDirectory(), false);
      strictEqual(ents[1].isFile(), true);
      strictEqual(ents[1].isBlockDevice(), false);
      strictEqual(ents[1].isCharacterDevice(), false);
      strictEqual(ents[1].isFIFO(), false);
      strictEqual(ents[1].isSocket(), false);
      strictEqual(ents[1].isSymbolicLink(), false);
      strictEqual(ents[1].parentPath, '/bundle');

      strictEqual(ents[4].name, 'dev/null');
      strictEqual(ents[4].isDirectory(), false);
      strictEqual(ents[4].isFile(), false);
      strictEqual(ents[4].isBlockDevice(), false);
      strictEqual(ents[4].isCharacterDevice(), true);
      strictEqual(ents[4].isFIFO(), false);
      strictEqual(ents[4].isSocket(), false);
      strictEqual(ents[4].isSymbolicLink(), false);
      strictEqual(ents[4].parentPath, '/dev');
    }
  },
};
