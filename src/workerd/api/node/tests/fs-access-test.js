// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ok, rejects, strictEqual, throws } from 'node:assert';

// Tests node:fs access and exists APIs
// These tests are not a straight up port of the Node.js tests but have
// been adapted and verified to match the behavior of the Node.js APIs
// as closely as possible.

import {
  accessSync,
  existsSync,
  access,
  exists,
  promises,
  constants,
  fstatSync,
  closeSync,
  openSync,
  writeSync,
  readSync,
} from 'node:fs';

const { F_OK, R_OK, W_OK, X_OK } = constants;

strictEqual(typeof accessSync, 'function');
strictEqual(typeof access, 'function');
strictEqual(typeof existsSync, 'function');
strictEqual(typeof exists, 'function');
strictEqual(typeof promises, 'object');
strictEqual(typeof promises.access, 'function');

strictEqual(typeof F_OK, 'number');
strictEqual(typeof R_OK, 'number');
strictEqual(typeof W_OK, 'number');
strictEqual(typeof X_OK, 'number');
strictEqual(F_OK, 0);
strictEqual(X_OK, 1);
strictEqual(W_OK, 2);
strictEqual(R_OK, 4);

const kKnownPaths = [
  { path: '/', writable: false },
  { path: '/bundle', writable: false },
  { path: '/bundle/worker', writable: false },
  { path: '/dev', writable: false },
  { path: '/dev/null', writable: true },
  { path: '/dev/zero', writable: true },
  { path: '/dev/full', writable: true },
  { path: '/dev/random', writable: true },
  { path: '/tmp', writable: true },
];

const UV_ENOENT = -2;

const kNoEntError = {
  code: 'ENOENT',
  errno: UV_ENOENT,
  syscall: 'access',
};

const kInvalidArgTypeError = { code: 'ERR_INVALID_ARG_TYPE' };
const kOutOfRangeError = { code: 'ERR_OUT_OF_RANGE' };

function mustNotCall() {
  throw new Error('This function should not be called');
}

export const accessInputValidationTest = {
  async test() {
    // For both sync and async callback versions, input validation errors
    // are thrown synchronously.
    [
      123n,
      {
        [Symbol.toPrimitive]() {
          return R_OK;
        },
      },
      '/',
    ].forEach((i) => {
      throws(() => accessSync('/', i), kInvalidArgTypeError);
      throws(() => access('/', i, mustNotCall), kInvalidArgTypeError);
      throws(() => access('/', F_OK, i), kInvalidArgTypeError);
    });
    throws(() => accessSync(123), kInvalidArgTypeError);
    throws(() => access(123, '', mustNotCall), kInvalidArgTypeError);

    // Mode is out of range
    throws(() => accessSync('/', -1), kOutOfRangeError);
    throws(
      () => accessSync('/', (F_OK | R_OK | W_OK | X_OK) + 1),
      kOutOfRangeError
    );

    // For the promise version, input validation errors cause rejected promises.
    await rejects(promises.access('/', 123n), kInvalidArgTypeError);
    await rejects(promises.access(123), kInvalidArgTypeError);
    await rejects(promises.access('/', -1), kOutOfRangeError);
    await rejects(
      promises.access('/', (F_OK | R_OK | W_OK | X_OK) + 1),
      kOutOfRangeError
    );
  },
};

export const accessSyncTest = {
  test() {
    kKnownPaths.forEach(({ path, writable }) => {
      const bufferPath = Buffer.from(path);
      const urlPath = new URL(path, 'file:///');
      accessSync(path);
      accessSync(bufferPath);
      accessSync(urlPath);
      accessSync(path, F_OK);
      accessSync(bufferPath, F_OK);
      accessSync(urlPath, F_OK);
      accessSync(path, R_OK);
      accessSync(bufferPath, R_OK);
      accessSync(urlPath, R_OK);

      // Some paths are writable (W_OK)
      if (writable) {
        accessSync(path, W_OK);
        accessSync(bufferPath, W_OK);
        accessSync(urlPath, W_OK);
      } else {
        throws(() => accessSync(path, W_OK), kNoEntError);
        throws(() => accessSync(bufferPath, W_OK), kNoEntError);
        throws(() => accessSync(urlPath, W_OK), kNoEntError);
      }

      // No path is executable (X_OK)
      throws(() => accessSync(path, X_OK), kNoEntError);
      throws(() => accessSync(bufferPath, X_OK), kNoEntError);
      throws(() => accessSync(urlPath, X_OK), kNoEntError);
    });

    // Paths that don't exist...
    throws(() => accessSync('/does/not/exist'), kNoEntError);
    throws(() => accessSync('/does/not/exist', F_OK), kNoEntError);
    throws(() => accessSync('/does/not/exist', R_OK), kNoEntError);
    throws(() => accessSync('/does/not/exist', W_OK), kNoEntError);
    throws(() => accessSync('/does/not/exist', X_OK), kNoEntError);
  },
};

export const accessCallbackTest = {
  async test() {
    const doTest = (path, mode) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      access(path, mode, (err) => {
        if (err) reject(err);
        else resolve();
      });
      return promise;
    };

    const cases = [];
    kKnownPaths.forEach(({ path, writable }) => {
      const bufferPath = Buffer.from(path);
      const urlPath = new URL(path, 'file:///');
      cases.push(
        doTest(path, F_OK),
        doTest(bufferPath, F_OK),
        doTest(urlPath, F_OK),
        doTest(path, R_OK),
        doTest(bufferPath, R_OK),
        doTest(urlPath, R_OK)
      );
      if (writable) {
        cases.push(
          doTest(path, W_OK),
          doTest(bufferPath, W_OK),
          doTest(urlPath, W_OK)
        );
      } else {
        cases.push(
          rejects(doTest(path, W_OK), kNoEntError),
          rejects(doTest(bufferPath, W_OK), kNoEntError),
          rejects(doTest(urlPath, W_OK), kNoEntError)
        );
      }
      // No paths are executable (X_OK)
      cases.push(
        rejects(doTest(path, X_OK), kNoEntError),
        rejects(doTest(bufferPath, X_OK), kNoEntError),
        rejects(doTest(urlPath, X_OK), kNoEntError)
      );
    });
    // Paths that don't exist have no permissions.
    cases.push(
      rejects(doTest('/does/not/exist', F_OK), kNoEntError),
      rejects(doTest('/does/not/exist', R_OK), kNoEntError),
      rejects(doTest('/does/not/exist', W_OK), kNoEntError),
      rejects(doTest('/does/not/exist', X_OK), kNoEntError)
    );
    strictEqual(cases.length, kKnownPaths.length * 12 + 4);
    await Promise.all(cases);
  },
};

export const accessPromiseTest = {
  async test() {
    const cases = [];
    kKnownPaths.forEach(({ path, writable }) => {
      const bufferPath = Buffer.from(path);
      const urlPath = new URL(path, 'file:///');
      cases.push(
        promises.access(path, F_OK),
        promises.access(bufferPath, F_OK),
        promises.access(urlPath, F_OK),
        promises.access(path, R_OK),
        promises.access(bufferPath, R_OK),
        promises.access(urlPath, R_OK)
      );
      if (writable) {
        cases.push(
          promises.access(path, W_OK),
          promises.access(bufferPath, W_OK),
          promises.access(urlPath, W_OK)
        );
      } else {
        cases.push(
          rejects(promises.access(path, W_OK), kNoEntError),
          rejects(promises.access(bufferPath, W_OK), kNoEntError),
          rejects(promises.access(urlPath, W_OK), kNoEntError)
        );
      }
      // No paths are executable (X_OK)
      cases.push(
        rejects(promises.access(path, X_OK), kNoEntError),
        rejects(promises.access(bufferPath, X_OK), kNoEntError),
        rejects(promises.access(urlPath, X_OK), kNoEntError)
      );
    });
    // Paths that don't exist have no permissions.
    cases.push(
      rejects(promises.access('/does/not/exist', F_OK), kNoEntError),
      rejects(promises.access('/does/not/exist', R_OK), kNoEntError),
      rejects(promises.access('/does/not/exist', W_OK), kNoEntError),
      rejects(promises.access('/does/not/exist', X_OK), kNoEntError)
    );
    strictEqual(cases.length, kKnownPaths.length * 12 + 4);
    await Promise.all(cases);
  },
};

export const existsSyncTest = {
  test() {
    kKnownPaths.forEach(({ path }) => {
      const bufferPath = Buffer.from(path);
      const urlPath = new URL(path, 'file:///');
      ok(existsSync(path));
      ok(existsSync(bufferPath));
      ok(existsSync(urlPath));
    });

    // Paths that don't exist
    ok(!existsSync('/does/not/exist'));
    // Incorrect inputs types results in false returned
    ok(!existsSync(123));
  },
};

export const existsCallbackTest = {
  async test() {
    const doTest = (path, expected) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      exists(path, (exists) => {
        if (exists !== expected)
          reject(new Error(`Expected ${path} to be ${expected}`));
        else resolve();
      });
      return promise;
    };

    const cases = [];
    kKnownPaths.forEach(({ path }) => {
      const bufferPath = Buffer.from(path);
      const urlPath = new URL(path, 'file:///');
      cases.push(
        doTest(path, true),
        doTest(bufferPath, true),
        doTest(urlPath, true)
      );
    });
    // Paths that don't exist
    cases.push(doTest('/does/not/exist', false));
    // Incorrect inputs types results in false returned
    cases.push(doTest(123, false));

    strictEqual(cases.length, kKnownPaths.length * 3 + 2);
    await Promise.all(cases);
  },
};

export const stdioFds = {
  test() {
    let stat1 = fstatSync(0);
    let stat2 = fstatSync(1);
    let stat3 = fstatSync(2);
    ok(stat1.isFile());
    ok(stat2.isFile());
    ok(stat3.isFile());
    closeSync(0);
    closeSync(1);
    closeSync(2);
    // After closing the stdio file descriptors, they can still be used.
    // That is, we don't actually close them.
    stat1 = fstatSync(0);
    stat2 = fstatSync(1);
    stat3 = fstatSync(2);
    ok(stat1.isFile());
    ok(stat2.isFile());
    ok(stat3.isFile());

    // Writing to the stdio file descriptors is permitted, always in append mode.
    // Position is ignored.
    writeSync(0, 'Hello, stdin!\n', 1000);
    writeSync(0, 'test', 500);
    writeSync(1, 'Hello, stdin!\n', 1000);
    writeSync(1, 'test', 500);
    writeSync(2, 'Hello, stdin!\n', 1000);
    writeSync(2, 'test', 500);

    stat1 = fstatSync(0);
    strictEqual(stat1.size, 0);

    const buf = Buffer.alloc(20);
    const bytesRead = readSync(0, buf, { position: 1000 });

    const fd = openSync('/tmp/test.txt', 'w');
    strictEqual(fd, 3);
  },
};
// There is no promise version of exists in Node.js, so no test for it here.
