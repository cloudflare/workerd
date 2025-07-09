import {
  WriteStream,
  createWriteStream,
  createReadStream,
  readFileSync,
  closeSync,
  open as openAsync,
  close as closeAsync,
  write as writeAsync,
  writev as writevAsync,
  fsync as fsyncAsync,
  promises,
} from 'node:fs';

import { Writable } from 'node:stream';

import {
  deepStrictEqual,
  ok,
  notStrictEqual,
  strictEqual,
  throws,
} from 'node:assert';
import { mock } from 'node:test';

strictEqual(typeof WriteStream, 'function');
strictEqual(typeof createWriteStream, 'function');

export const simpleWriteStreamTest = {
  async test() {
    const path = '/tmp/workerd-fs-writestream-test.txt';
    const data = 'Hello, World!';
    const writeStream = createWriteStream(path, { flags: 'w' });
    writeStream.write(data);
    writeStream.end();
    const { promise, resolve } = Promise.withResolvers();
    const { promise: finishPromise, resolve: finishResolve } =
      Promise.withResolvers();
    writeStream.on('finish', finishResolve);
    writeStream.on('close', resolve);
    await Promise.all([promise, finishPromise]);
    const check = readFileSync(path, 'utf8');
    strictEqual(check, data);
  },
};

export const writeStreamTest1 = {
  async test() {
    const path = '/tmp/workerd-fs-writestream-test1.txt';
    const { promise, resolve } = Promise.withResolvers();
    const stream = WriteStream(path, {
      fs: {
        close(fd) {
          ok(fd);
          closeSync(fd);
          resolve();
        },
      },
    });
    stream.destroy();
    await promise;
  },
};

export const writeStreamTest2 = {
  async test() {
    const path = '/tmp/workerd-fs-writestream-test2.txt';
    const stream = createWriteStream(path);

    const { promise, resolve, reject } = Promise.withResolvers();

    stream.on('drain', reject);
    stream.on('close', resolve);
    stream.destroy();
    await promise;
  },
};

export const writeStreamTest3 = {
  async test() {
    const path = '/tmp/workerd-fs-writestream-test3.txt';
    const stream = createWriteStream(path);
    const { promise, resolve, reject } = Promise.withResolvers();
    stream.on('error', reject);
    stream.on('close', resolve);
    throws(() => stream.write(42), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });
    stream.destroy();
    await promise;
  },
};

export const writeStreamTest4 = {
  test() {
    const example = '/tmp/workerd-fs-writestream-test4.txt';
    createWriteStream(example, undefined).end();
    createWriteStream(example, null).end();
    createWriteStream(example, 'utf8').end();
    createWriteStream(example, { encoding: 'utf8' }).end();

    const createWriteStreamErr = (path, opt) => {
      throws(() => createWriteStream(path, opt), {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      });
    };

    createWriteStreamErr(example, 123);
    createWriteStreamErr(example, 0);
    createWriteStreamErr(example, true);
    createWriteStreamErr(example, false);
  },
};

export const writeStreamTest5 = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    WriteStream.prototype.open = resolve;
    createWriteStream('/tmp/test');
    await promise;
    delete WriteStream.prototype.open;
  },
};

export const writeStreamTest6 = {
  async test() {
    const path = '/tmp/write-end-test0.txt';
    const fs = {
      open: mock.fn(openAsync),
      write: mock.fn(writeAsync),
      close: mock.fn(closeAsync),
    };
    const { promise, resolve } = Promise.withResolvers();
    const stream = createWriteStream(path, { fs });
    stream.on('close', resolve);
    stream.end('asd');

    await promise;
    strictEqual(fs.open.mock.callCount(), 1);
    strictEqual(fs.write.mock.callCount(), 1);
    strictEqual(fs.close.mock.callCount(), 1);
  },
};

export const writeStreamTest7 = {
  async test() {
    const path = '/tmp/write-end-test1.txt';
    const fs = {
      open: mock.fn(openAsync),
      write: writeAsync,
      writev: mock.fn(writevAsync),
      close: mock.fn(closeAsync),
    };
    const stream = createWriteStream(path, { fs });
    stream.write('asd');
    stream.write('asd');
    stream.write('asd');
    stream.end();
    const { promise, resolve } = Promise.withResolvers();
    stream.on('close', resolve);
    await promise;

    strictEqual(fs.open.mock.callCount(), 1);
    strictEqual(fs.writev.mock.callCount(), 1);
    strictEqual(fs.close.mock.callCount(), 1);
  },
};

let cnt = 0;
function nextFile() {
  return `/tmp/${cnt++}.out`;
}

export const writeStreamTest8 = {
  test() {
    for (const flush of ['true', '', 0, 1, [], {}, Symbol()]) {
      throws(
        () => {
          createWriteStream(nextFile(), { flush });
        },
        { code: 'ERR_INVALID_ARG_TYPE' }
      );
    }
  },
};

export const writeStreamTest9 = {
  async test() {
    const fs = {
      fsync: mock.fn(fsyncAsync),
    };
    const stream = createWriteStream(nextFile(), { flush: true, fs });

    const { promise, resolve, reject } = Promise.withResolvers();

    stream.write('hello', (err) => {
      if (err) return reject();
      stream.close((err) => {
        if (err) return reject(err);
        resolve();
      });
    });

    await promise;

    strictEqual(fs.fsync.mock.callCount(), 1);
  },
};

export const writeStreamTest10 = {
  async test() {
    const values = [undefined, null, false];
    const fs = {
      fsync: mock.fn(fsyncAsync),
    };
    let cnt = 0;

    const { promise, resolve, reject } = Promise.withResolvers();

    for (const flush of values) {
      const file = nextFile();
      const stream = createWriteStream(file, { flush });
      stream.write('hello world', (err) => {
        if (err) return reject(err);
        stream.close((err) => {
          if (err) return reject(err);
          strictEqual(readFileSync(file, 'utf8'), 'hello world');
          cnt++;
          if (cnt === values.length) {
            strictEqual(fs.fsync.mock.callCount(), 0);
            resolve();
          }
        });
      });
    }

    await promise;
  },
};

export const writeStreamTest11 = {
  async test() {
    const file = nextFile();
    const handle = await promises.open(file, 'w');
    const stream = handle.createWriteStream({ flush: true });

    const { promise, resolve, reject } = Promise.withResolvers();

    stream.write('hello', (err) => {
      if (err) return reject(err);
      stream.close((err) => {
        if (err) return reject(err);
        strictEqual(readFileSync(file, 'utf8'), 'hello');
        resolve();
      });
    });

    await promise;
  },
};

export const writeStreamTest12 = {
  async test() {
    const file = nextFile();
    const handle = await promises.open(file, 'w+');

    const { promise, resolve } = Promise.withResolvers();
    handle.on('close', resolve);
    const stream = createWriteStream(null, { fd: handle });

    stream.end('hello');
    stream.on('close', () => {
      const output = readFileSync(file, 'utf-8');
      strictEqual(output, 'hello');
    });

    await promise;
  },
};

export const writeStreamTest13 = {
  async test() {
    const file = nextFile();
    const handle = await promises.open(file, 'w+');
    let calls = 0;
    const { write: originalWriteFunction, writev: originalWritevFunction } =
      handle;
    handle.write = mock.fn(handle.write.bind(handle));
    handle.writev = mock.fn(handle.writev.bind(handle));
    const stream = createWriteStream(null, { fd: handle });
    stream.end('hello');
    const { promise, resolve } = Promise.withResolvers();
    stream.on('close', () => {
      console.log('test');
      ok(handle.write.mock.callCount() + handle.writev.mock.callCount() > 0);
      resolve();
    });
    await promise;
  },
};

export const writeStreamTest14 = {
  async test() {
    const path = '/tmp/out';

    let writeCalls = 0;
    const fs = {
      write: mock.fn((...args) => {
        switch (writeCalls++) {
          case 0: {
            return writeAsync(...args);
          }
          case 1: {
            args[args.length - 1](new Error('BAM'));
            break;
          }
          default: {
            // It should not be called again!
            throw new Error('BOOM!');
          }
        }
      }),
      close: mock.fn(closeAsync),
    };

    const stream = createWriteStream(path, {
      highWaterMark: 10,
      fs,
    });

    const { promise: errorPromise, resolve: errorResolve } =
      Promise.withResolvers();
    const { promise: writePromise, resolve: writeResolve } =
      Promise.withResolvers();

    stream.on('error', (err) => {
      strictEqual(stream.fd, null);
      strictEqual(err.message, 'BAM');
      errorResolve();
    });

    stream.write(Buffer.allocUnsafe(256), () => {
      stream.write(Buffer.allocUnsafe(256), (err) => {
        strictEqual(err.message, 'BAM');
        writeResolve();
      });
    });

    await Promise.all([errorPromise, writePromise]);
  },
};

export const writeStreamTest15 = {
  async test() {
    const file = '/tmp/write-end-test0.txt';
    const stream = createWriteStream(file);
    stream.end();
    const { promise, resolve } = Promise.withResolvers();
    stream.on('close', resolve);
    await promise;
  },
};

export const writeStreamTest16 = {
  async test() {
    const file = '/tmp/write-end-test1.txt';
    const stream = createWriteStream(file);
    stream.end('a\n', 'utf8');
    const { promise, resolve } = Promise.withResolvers();
    stream.on('close', () => {
      const content = readFileSync(file, 'utf8');
      strictEqual(content, 'a\n');
      resolve();
    });
    await promise;
  },
};

export const writeStreamTest17 = {
  async test() {
    const file = '/tmp/write-end-test2.txt';
    const stream = createWriteStream(file);
    stream.end();

    const { promise: openPromise, resolve: openResolve } =
      Promise.withResolvers();
    const { promise: finishPromise, resolve: finishResolve } =
      Promise.withResolvers();
    stream.on('open', openResolve);
    stream.on('finish', finishResolve);
    await Promise.all([openPromise, finishPromise]);
  },
};

export const writeStreamTest18 = {
  async test() {
    const examplePath = '/tmp/a';
    const dummyPath = '/tmp/b';
    const firstEncoding = 'base64';
    const secondEncoding = 'latin1';

    const exampleReadStream = createReadStream(examplePath, {
      encoding: firstEncoding,
    });

    const dummyWriteStream = createWriteStream(dummyPath, {
      encoding: firstEncoding,
    });

    const { promise, resolve } = Promise.withResolvers();
    exampleReadStream.pipe(dummyWriteStream).on('finish', () => {
      const assertWriteStream = new Writable({
        write: function (chunk, enc, next) {
          const expected = Buffer.from('xyz\n');
          deepStrictEqual(expected, chunk);
        },
      });
      assertWriteStream.setDefaultEncoding(secondEncoding);
      createReadStream(dummyPath, {
        encoding: secondEncoding,
      })
        .pipe(assertWriteStream)
        .on('close', resolve);
    });

    await promise;
  },
};

export const writeStreamTest19 = {
  async test() {
    const file = '/tmp/write-end-test3.txt';
    const stream = createWriteStream(file);
    const { promise: closePromise1, resolve: closeResolve1 } =
      Promise.withResolvers();
    const { promise: closePromise2, resolve: closeResolve2 } =
      Promise.withResolvers();
    stream.close(closeResolve1);
    stream.close(closeResolve2);
    await Promise.all([closePromise1, closePromise2]);
  },
};

export const writeStreamTest20 = {
  async test() {
    const file = '/tmp/write-autoclose-opt1.txt';
    let stream = createWriteStream(file, { flags: 'w+', autoClose: false });
    stream.write('Test1');
    stream.end();
    const { promise, resolve, reject } = Promise.withResolvers();
    stream.on('finish', () => {
      stream.on('close', reject);
      process.nextTick(() => {
        strictEqual(stream.closed, false);
        notStrictEqual(stream.fd, null);
        resolve();
      });
    });
    await promise;

    const { promise: nextPromise, resolve: nextResolve } =
      Promise.withResolvers();
    const stream2 = createWriteStream(null, { fd: stream.fd, start: 0 });
    stream2.write('Test2');
    stream2.end();
    stream2.on('finish', () => {
      strictEqual(stream2.closed, false);
      stream2.on('close', () => {
        strictEqual(stream2.fd, null);
        strictEqual(stream2.closed, true);
        nextResolve();
      });
    });

    await nextPromise;

    const data = readFileSync(file, 'utf8');
    strictEqual(data, 'Test2');
  },
};

export const writeStreamTest21 = {
  async test() {
    // This is to test success scenario where autoClose is true
    const file = '/tmp/write-autoclose-opt2.txt';
    const stream = createWriteStream(file, { autoClose: true });
    stream.write('Test3');
    stream.end();
    const { promise, resolve } = Promise.withResolvers();
    stream.on('finish', () => {
      strictEqual(stream.closed, false);
      stream.on('close', () => {
        strictEqual(stream.fd, null);
        strictEqual(stream.closed, true);
        resolve();
      });
    });
    await promise;
  },
};

export const writeStreamTest22 = {
  test() {
    throws(() => WriteStream.prototype.autoClose, {
      code: 'ERR_INVALID_THIS',
    });
  },
};
