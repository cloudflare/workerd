// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { rejects, strictEqual, throws } from 'node:assert';

// Tests node:fs chmod and chown APIS
// These tests are not a straight up port of the Node.js tests but have
// been adapted and verified to match the behavior of the Node.js APIs
// as closely as possible.

// Our implementation of these APIs is mostly a non-op. The most we do
// is validate the input arguments and verify that the file exists and
// is accessible. The actual chown and chmod operations are not performed
// since we do not have a concept of users, groups, or posix-style permissions.

import {
  openSync,
  closeSync,
  statSync,
  fstatSync,
  lstatSync,
  symlinkSync,
  chmod,
  lchmod,
  fchmod,
  chmodSync,
  lchmodSync,
  fchmodSync,
  chown,
  lchown,
  fchown,
  chownSync,
  lchownSync,
  fchownSync,
  promises,
} from 'node:fs';

strictEqual(typeof openSync, 'function');
strictEqual(typeof closeSync, 'function');
strictEqual(typeof statSync, 'function');
strictEqual(typeof fstatSync, 'function');
strictEqual(typeof lstatSync, 'function');
strictEqual(typeof symlinkSync, 'function');
strictEqual(typeof chmod, 'function');
strictEqual(typeof lchmod, 'function');
strictEqual(typeof fchmod, 'function');
strictEqual(typeof chmodSync, 'function');
strictEqual(typeof lchmodSync, 'function');
strictEqual(typeof fchmodSync, 'function');
strictEqual(typeof chown, 'function');
strictEqual(typeof lchown, 'function');
strictEqual(typeof fchown, 'function');
strictEqual(typeof chownSync, 'function');
strictEqual(typeof lchownSync, 'function');
strictEqual(typeof fchownSync, 'function');
strictEqual(typeof promises.chown, 'function');
strictEqual(typeof promises.lchown, 'function');

const kInvalidArgTypeError = { code: 'ERR_INVALID_ARG_TYPE' };
const kOutOfRangeError = { code: 'ERR_OUT_OF_RANGE' };

function checkStat(path) {
  const { uid, gid } = statSync(path);
  strictEqual(uid, 0);
  strictEqual(gid, 0);
}

function checkfStat(fd) {
  const { uid, gid } = fstatSync(fd);
  strictEqual(uid, 0);
  strictEqual(gid, 0);
}

const path = '/tmp';
const bufferPath = Buffer.from(path);
const urlPath = new URL(path, 'file:///');

export const chownSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => chownSync(123), kInvalidArgTypeError);
    throws(() => chownSync('/', {}), kInvalidArgTypeError);
    throws(() => chownSync('/', 0, {}), kInvalidArgTypeError);
    throws(() => chownSync(path, -1000, 0), kOutOfRangeError);
    throws(() => chownSync(path, 0, -1000), kOutOfRangeError);

    // We stat the file before and after to verify the impact
    // of the chown operation. Specifically, the uid and gid
    // should not change since our impl is a non-op.
    checkStat(path);
    chownSync(path, 1000, 1000);
    checkStat(path);

    chownSync(bufferPath, 1000, 1000);
    checkStat(bufferPath);

    chownSync(urlPath, 1000, 1000);
    checkStat(urlPath);

    // A non-existent path should throw ENOENT
    throws(() => chownSync('/non-existent-path', 1000, 1000), {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'chown',
    });
  },
};

export const chownCallbackTest = {
  async test() {
    // Incorrect input types should throw synchronously
    throws(() => chown(123), kInvalidArgTypeError);
    throws(() => chown('/', {}), kInvalidArgTypeError);
    throws(() => chown('/', 0, {}), kInvalidArgTypeError);
    throws(() => chownSync(path, -1000, 0), kOutOfRangeError);
    throws(() => chownSync(path, 0, -1000), kOutOfRangeError);

    async function callChown(path) {
      const { promise, resolve, reject } = Promise.withResolvers();
      chown(path, 1000, 1000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    // Should be non-op
    checkStat(path);
    await callChown(path);
    checkStat(path);

    await callChown(bufferPath);
    checkStat(bufferPath);

    await callChown(urlPath);
    checkStat(urlPath);

    // A non-existent path should throw ENOENT
    const { promise, resolve, reject } = Promise.withResolvers();
    chown('/non-existent-path', 1000, 1000, (err) => {
      if (err) return reject(err);
      resolve();
    });
    await rejects(promise, {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'chown',
    });
  },
};

export const chownPromiseTest = {
  async test() {
    // Incorrect input types should reject the promise.
    await rejects(promises.chown(123), kInvalidArgTypeError);
    await rejects(promises.chown('/', {}), kInvalidArgTypeError);
    await rejects(promises.chown('/', 0, {}), kInvalidArgTypeError);
    await rejects(promises.chown(path, -1000, 0), kOutOfRangeError);
    await rejects(promises.chown(path, 0, -1000), kOutOfRangeError);

    // Should be non-op
    checkStat(path);
    await promises.chown(path, 1000, 1000);
    checkStat(path);

    await promises.chown(bufferPath, 1000, 1000);
    checkStat(bufferPath);

    await promises.chown(urlPath, 1000, 1000);
    checkStat(urlPath);

    // A non-existent path should throw ENOENT
    await rejects(promises.chown('/non-existent-path', 1000, 1000), {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'chown',
    });
  },
};

export const lchownSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => lchownSync(123), kInvalidArgTypeError);
    throws(() => lchownSync('/', {}), kInvalidArgTypeError);
    throws(() => lchownSync('/', 0, {}), kInvalidArgTypeError);
    throws(() => lchownSync(path, -1000, 0), kOutOfRangeError);
    throws(() => lchownSync(path, 0, -1000), kOutOfRangeError);

    // We stat the file before and after to verify the impact
    // of the chown operation. Specifically, the uid and gid
    // should not change since our impl is a non-op.
    checkStat(path);
    lchownSync(path, 1000, 1000);
    checkStat(path);

    lchownSync(bufferPath, 1000, 1000);
    checkStat(bufferPath);

    lchownSync(urlPath, 1000, 1000);
    checkStat(urlPath);

    // A non-existent path should throw ENOENT
    throws(() => lchownSync('/non-existent-path', 1000, 1000), {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'lchown',
    });
  },
};

export const lchownCallbackTest = {
  async test() {
    // Incorrect input types should throw synchronously
    throws(() => lchown(123), kInvalidArgTypeError);
    throws(() => lchown('/', {}), kInvalidArgTypeError);
    throws(() => lchown('/', 0, {}), kInvalidArgTypeError);
    throws(() => lchownSync(path, -1000, 0), kOutOfRangeError);
    throws(() => lchownSync(path, 0, -1000), kOutOfRangeError);

    async function callChown(path) {
      const { promise, resolve, reject } = Promise.withResolvers();
      lchown(path, 1000, 1000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    // Should be non-op
    checkStat(path);
    await callChown(path);
    checkStat(path);

    await callChown(bufferPath);
    checkStat(bufferPath);

    await callChown(urlPath);
    checkStat(urlPath);

    // A non-existent path should throw ENOENT
    const { promise, resolve, reject } = Promise.withResolvers();
    lchown('/non-existent-path', 1000, 1000, (err) => {
      if (err) return reject(err);
      resolve();
    });
    await rejects(promise, {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'lchown',
    });
  },
};

export const lchownPromiseTest = {
  async test() {
    // Incorrect input types should reject the promise.
    await rejects(promises.lchown(123), kInvalidArgTypeError);
    await rejects(promises.lchown('/', {}), kInvalidArgTypeError);
    await rejects(promises.lchown('/', 0, {}), kInvalidArgTypeError);
    await rejects(promises.lchown(path, -1000, 0), kOutOfRangeError);
    await rejects(promises.lchown(path, 0, -1000), kOutOfRangeError);

    // Should be non-op
    checkStat(path);
    await promises.lchown(path, 1000, 1000);
    checkStat(path);

    await promises.lchown(bufferPath, 1000, 1000);
    checkStat(bufferPath);

    await promises.lchown(urlPath, 1000, 1000);
    checkStat(urlPath);

    // A non-existent path should throw ENOENT
    await rejects(promises.lchown('/non-existent-path', 1000, 1000), {
      code: 'ENOENT',
      syscall: 'lchown',
    });
  },
};

export const fchownSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => fchownSync({}), kInvalidArgTypeError);
    throws(() => fchownSync(123), kInvalidArgTypeError);
    throws(() => fchownSync(123, {}), kInvalidArgTypeError);
    throws(() => fchownSync(123, 0, {}), kInvalidArgTypeError);
    throws(() => fchownSync(123, -1000, 0), kOutOfRangeError);
    throws(() => fchownSync(123, 0, -1000), kOutOfRangeError);

    const fd = openSync('/tmp');

    // We stat the file before and after to verify the impact
    // of the chown operation. Specifically, the uid and gid
    // should not change since our impl is a non-op.
    checkfStat(fd);
    fchownSync(fd, 1000, 1000);
    checkfStat(fd);

    throws(() => fchownSync(999, 1000, 1000), {
      code: 'EBADF',
      syscall: 'fstat',
    });

    closeSync(fd);
  },
};

export const fchownCallbackTest = {
  async test() {
    // Incorrect input types should throw synchronously
    throws(() => fchown({}), kInvalidArgTypeError);
    throws(() => fchown(123), kInvalidArgTypeError);
    throws(() => fchown(123, {}), kInvalidArgTypeError);
    throws(() => fchown(123, 0, {}), kInvalidArgTypeError);
    throws(() => fchown(123, -1000, 0), kOutOfRangeError);
    throws(() => fchown(123, 0, -1000), kOutOfRangeError);

    const fd = openSync('/tmp');

    async function callChown() {
      const { promise, resolve, reject } = Promise.withResolvers();
      fchown(fd, 1000, 1000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    // Should be non-op
    checkfStat(fd);
    await callChown();
    checkfStat(fd);

    const { promise, resolve, reject } = Promise.withResolvers();
    fchown(999, 1000, 1000, (err) => {
      if (err) return reject(err);
      resolve();
    });
    await rejects(promise, {
      code: 'EBADF',
      syscall: 'fstat',
    });

    closeSync(fd);
  },
};

// ===========================================================================

export const chmodSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => chmodSync(123), kInvalidArgTypeError);
    throws(() => chmodSync('/', {}), kInvalidArgTypeError);
    throws(() => chmodSync('/tmp', -1), kOutOfRangeError);

    // Should be non-op
    checkStat(path);
    chmodSync(path, 0o777);
    checkStat(path);

    chmodSync(bufferPath, 0o777);
    checkStat(bufferPath);

    chmodSync(urlPath, 0o777);
    checkStat(urlPath);

    throws(() => chmodSync('/non-existent-path', 0o777), {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'chmod',
    });
  },
};

export const chmodCallbackTest = {
  async test() {
    // Incorrect input types should throw.
    throws(() => chmod(123), kInvalidArgTypeError);
    throws(() => chmod('/', {}), kInvalidArgTypeError);
    throws(() => chmod('/tmp', -1), kOutOfRangeError);

    async function callChmod(path) {
      const { promise, resolve, reject } = Promise.withResolvers();
      chmod(path, 0o000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    checkStat(path);
    await callChmod(path);
    checkStat(path);

    await callChmod(bufferPath);
    checkStat(bufferPath);

    await callChmod(urlPath);
    checkStat(bufferPath);

    const { promise, resolve, reject } = Promise.withResolvers();
    chmod('/non-existent-path', 0o777, (err) => {
      if (err) return reject(err);
      resolve();
    });
    await rejects(promise, {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'chmod',
    });
  },
};

export const chmodPromiseTest = {
  async test() {
    // Incorrect input types should reject the promise.
    await rejects(promises.chmod(123), kInvalidArgTypeError);
    await rejects(promises.chmod('/', {}), kInvalidArgTypeError);
    await rejects(promises.chmod('/tmp', -1), kOutOfRangeError);

    // Should be non-op
    checkStat(path);
    await promises.chmod(path, 0o777);
    checkStat(path);

    await promises.chmod(bufferPath, 0o777);
    checkStat(bufferPath);

    await promises.chmod(urlPath, 0o777);
    checkStat(urlPath);

    await rejects(promises.chmod('/non-existent-path', 0o777), {
      code: 'ENOENT',
      syscall: 'chmod',
    });
  },
};

export const lchmodSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => lchmodSync(123), kInvalidArgTypeError);
    throws(() => lchmodSync('/', {}), kInvalidArgTypeError);
    throws(() => lchmodSync('/tmp', -1), kOutOfRangeError);

    // Should be non-op
    checkStat(path);
    lchmodSync(path, 0o777);
    checkStat(path);

    lchmodSync(bufferPath, 0o777);
    checkStat(bufferPath);

    lchmodSync(urlPath, 0o777);
    checkStat(urlPath);

    throws(() => lchmodSync('/non-existent-path', 0o777), {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'lchmod',
    });
  },
};

export const lchmodCallbackTest = {
  async test() {
    // Incorrect input types should throw.
    throws(() => lchmod(123), kInvalidArgTypeError);
    throws(() => lchmod('/', {}), kInvalidArgTypeError);
    throws(() => lchmod('/tmp', -1), kOutOfRangeError);

    async function callChmod(path) {
      const { promise, resolve, reject } = Promise.withResolvers();
      lchmod(path, 0o000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    checkStat(path);
    await callChmod(path);
    checkStat(path);

    await callChmod(bufferPath);
    checkStat(bufferPath);

    await callChmod(urlPath);
    checkStat(bufferPath);

    const { promise, resolve, reject } = Promise.withResolvers();
    lchmod('/non-existent-path', 0o777, (err) => {
      if (err) return reject(err);
      resolve();
    });
    await rejects(promise, {
      code: 'ENOENT',
      // Access because it is an access check under the covers.
      syscall: 'lchmod',
    });
  },
};

export const lchmodPromiseTest = {
  async test() {
    // Incorrect input types should reject the promise.
    await rejects(promises.lchmod(123), kInvalidArgTypeError);
    await rejects(promises.lchmod('/', {}), kInvalidArgTypeError);
    await rejects(promises.lchmod('/tmp', -1), kOutOfRangeError);

    // Should be non-op
    checkStat(path);
    await promises.lchmod(path, 0o777);
    checkStat(path);

    await promises.lchmod(bufferPath, 0o777);
    checkStat(bufferPath);

    await promises.lchmod(urlPath, 0o777);
    checkStat(urlPath);

    await rejects(promises.lchmod('/non-existent-path', 0o777), {
      code: 'ENOENT',
      syscall: 'lchmod',
    });
  },
};

export const fchmodSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => fchmodSync({}), kInvalidArgTypeError);
    throws(() => fchmodSync(123), kInvalidArgTypeError);
    throws(() => fchmodSync(123, {}), kInvalidArgTypeError);
    throws(() => fchmodSync(123, -1000), kOutOfRangeError);

    const fd = openSync('/tmp');

    // We stat the file before and after to verify the impact
    // of the chown operation. Specifically, the uid and gid
    // should not change since our impl is a non-op.
    checkfStat(fd);
    fchmodSync(fd, 0o777);
    checkfStat(fd);

    throws(() => fchmodSync(999, 0o777), {
      code: 'EBADF',
      syscall: 'fstat',
    });

    closeSync(fd);
  },
};

export const fchmodCallbackTest = {
  async test() {
    // Incorrect input types should throw synchronously
    throws(() => fchmod({}), kInvalidArgTypeError);
    throws(() => fchmod(123), kInvalidArgTypeError);
    throws(() => fchmod(123, {}), kInvalidArgTypeError);

    const fd = openSync('/tmp');

    async function callChmod() {
      const { promise, resolve, reject } = Promise.withResolvers();
      fchmod(fd, 0o777, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    // Should be non-op
    checkfStat(fd);
    await callChmod();
    checkfStat(fd);

    const { promise, resolve, reject } = Promise.withResolvers();
    fchmod(999, 0o777, (err) => {
      if (err) return reject(err);
      resolve();
    });
    await rejects(promise, {
      code: 'EBADF',
      syscall: 'fstat',
    });

    closeSync(fd);
  },
};
