// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ifError, ok, rejects, strictEqual, throws } from 'node:assert';

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
  // TODO(node-fs): Uncomment when relevant tests are provided.
  // readdir,
  // readlink,
  // realpath,
  // mkdtemp,
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
} from 'node:fs';

const { COPYFILE_EXCL } = constants;

const kErrInvalidArgType = { code: 'ERR_INVALID_ARG_TYPE' };
const kErrInvalidArgValue = { code: 'ERR_INVALID_ARG_VALUE' };
const kErrEBadf = { code: 'EBADF' };
const kErrEExist = { code: 'EEXIST' };
const kErrOutOfRange = { code: 'ERR_OUT_OF_RANGE' };
const kErrENoEntError = { code: 'ENOENT' };

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
      // Open the file
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
      const { promise, resolve, reject } = Promise.withResolvers();
      ftruncate(fd, -1, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await rejects(promise, kErrOutOfRange);
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
    throws(() => truncateSync('/tmp/test.txt', 10), kErrENoEntError);

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
    throws(
      () => writeSync(fd, Buffer.alloc(2), { length: 5 }),
      kErrInvalidArgValue
    );
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
    throws(
      () => write(fd, Buffer.alloc(2), { length: 5 }, mustNotCall),
      kErrInvalidArgValue
    );
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
      { code: 'ENOENT' }
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

    // TODO(node-fs): Enable these tests once encoding is verified correctly
    // throws(() => readFile('/tmp/test.txt', 'bad-encoding', mustNotCall), kErrorObj);
    // throws(() => readdir('/tmp/test.txt', 'bad-encoding', mustNotCall), kErrorObj);
    // throws(() => readlink('/tmp/test.txt', 'bad-encoding', mustNotCall), kErrorObj);
    // throws(() => realpath('/tmp/test.txt', 'bad-encoding', mustNotCall), kErrorObj);
    // throws(() => mkdtemp('/tmp/test.txt', 'bad-encoding', mustNotCall), kErrorObj);
  },
};
