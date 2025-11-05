// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, ok, rejects, strictEqual, throws } from 'node:assert';

import {
  openSync,
  closeSync,
  symlinkSync,
  statSync,
  lstatSync,
  fstatSync,
  stat,
  lstat,
  fstat,
  promises,
  statfsSync,
  statfs,
  Stats,
} from 'node:fs';

strictEqual(typeof openSync, 'function');
strictEqual(typeof closeSync, 'function');
strictEqual(typeof statSync, 'function');
strictEqual(typeof lstatSync, 'function');
strictEqual(typeof fstatSync, 'function');
strictEqual(typeof stat, 'function');
strictEqual(typeof lstat, 'function');
strictEqual(typeof fstat, 'function');
strictEqual(typeof symlinkSync, 'function');
strictEqual(typeof statfsSync, 'function');
strictEqual(typeof statfs, 'function');
strictEqual(typeof promises.stat, 'function');
strictEqual(typeof promises.lstat, 'function');

const kInvalidArgTypeError = { code: 'ERR_INVALID_ARG_TYPE' };

function checkRootStat(stat, bigint = false) {
  ok(stat instanceof Stats);
  strictEqual(stat.isDirectory(), true);
  strictEqual(stat.isFile(), false);
  strictEqual(stat.isBlockDevice(), false);
  strictEqual(stat.isCharacterDevice(), false);
  strictEqual(stat.isSymbolicLink(), false);
  strictEqual(stat.isFIFO(), false);
  strictEqual(stat.isSocket(), false);
  strictEqual(stat.dev, bigint ? 0n : 0);
  strictEqual(stat.ino, bigint ? 0n : 0);
  strictEqual(stat.mode, bigint ? 16676n : 16676);
  strictEqual(stat.nlink, bigint ? 1n : 1);
  strictEqual(stat.uid, bigint ? 0n : 0);
  strictEqual(stat.gid, bigint ? 0n : 0);
  strictEqual(stat.rdev, bigint ? 0n : 0);
  strictEqual(stat.size, bigint ? 0n : 0);
  strictEqual(stat.blksize, bigint ? 0n : 0);
  strictEqual(stat.blocks, bigint ? 0n : 0);
  strictEqual(stat.atime.getTime(), 0);
  strictEqual(stat.mtime.getTime(), 0);
  strictEqual(stat.ctime.getTime(), 0);
  strictEqual(stat.birthtime.getTime(), 0);
  strictEqual(stat.atimeMs, bigint ? 0n : 0);
  strictEqual(stat.mtimeMs, bigint ? 0n : 0);
  strictEqual(stat.ctimeMs, bigint ? 0n : 0);
  strictEqual(stat.birthtimeMs, bigint ? 0n : 0);
  if (bigint) {
    strictEqual(stat.atimeNs, 0n);
    strictEqual(stat.mtimeNs, 0n);
    strictEqual(stat.ctimeNs, 0n);
    strictEqual(stat.birthtimeNs, 0n);
  }
}

function checkFileStat(stat, bigint = false) {
  ok(stat instanceof Stats);
  strictEqual(stat.isDirectory(), false);
  strictEqual(stat.isFile(), true);
  strictEqual(stat.isBlockDevice(), false);
  strictEqual(stat.isCharacterDevice(), false);
  strictEqual(stat.isSymbolicLink(), false);
  strictEqual(stat.isFIFO(), false);
  strictEqual(stat.isSocket(), false);
  strictEqual(stat.dev, bigint ? 0n : 0);
  strictEqual(stat.ino, bigint ? 0n : 0);
  strictEqual(stat.mode, bigint ? 33060n : 33060);
  strictEqual(stat.nlink, bigint ? 1n : 1);
  strictEqual(stat.uid, bigint ? 0n : 0);
  strictEqual(stat.gid, bigint ? 0n : 0);
  strictEqual(stat.rdev, bigint ? 0n : 0);
  ok(stat.size > 0n || stat.size > 0);
  strictEqual(stat.blksize, bigint ? 0n : 0);
  strictEqual(stat.blocks, bigint ? 0n : 0);
  // The size is going to be constantly changing for this test file,
  // so let's just check that it's not zero.
}

function checkCharacterDeviceStat(stat, bigint = false) {
  ok(stat instanceof Stats);
  strictEqual(stat.isDirectory(), false);
  strictEqual(stat.isFile(), false);
  strictEqual(stat.isBlockDevice(), false);
  strictEqual(stat.isCharacterDevice(), true);
  strictEqual(stat.isSymbolicLink(), false);
  strictEqual(stat.isFIFO(), false);
  strictEqual(stat.isSocket(), false);
  strictEqual(stat.dev, bigint ? 1n : 1);
  strictEqual(stat.ino, bigint ? 0n : 0);
  strictEqual(stat.mode, bigint ? 8630n : 8630);
  strictEqual(stat.nlink, bigint ? 1n : 1);
  strictEqual(stat.uid, bigint ? 0n : 0);
  strictEqual(stat.gid, bigint ? 0n : 0);
  strictEqual(stat.rdev, bigint ? 0n : 0);
}

function checkLinkStat(stat, bigint = false) {
  ok(stat instanceof Stats);
  strictEqual(stat.isDirectory(), false);
  strictEqual(stat.isFile(), false);
  strictEqual(stat.isBlockDevice(), false);
  strictEqual(stat.isCharacterDevice(), false);
  strictEqual(stat.isSymbolicLink(), true);
  strictEqual(stat.isFIFO(), false);
  strictEqual(stat.isSocket(), false);
  strictEqual(stat.dev, bigint ? 0n : 0);
  strictEqual(stat.ino, bigint ? 0n : 0);
  strictEqual(stat.mode, bigint ? 41252n : 41252);
  strictEqual(stat.nlink, bigint ? 1n : 1);
  strictEqual(stat.uid, bigint ? 0n : 0);
  strictEqual(stat.gid, bigint ? 0n : 0);
  strictEqual(stat.rdev, bigint ? 0n : 0);
}

export const statSyncTest = {
  async test() {
    throws(() => statSync(), kInvalidArgTypeError);
    throws(() => statSync('/', 'foo'), kInvalidArgTypeError);
    throws(() => statSync('/', { bigint: 'yes' }), kInvalidArgTypeError);
    throws(() => lstatSync(), kInvalidArgTypeError);
    throws(() => lstatSync('/', 'foo'), kInvalidArgTypeError);
    throws(() => lstatSync('/', { bigint: 'yes' }), kInvalidArgTypeError);
    throws(() => fstatSync(), kInvalidArgTypeError);
    throws(() => fstatSync(123, 'foo'), kInvalidArgTypeError);
    throws(() => fstatSync(123, { bigint: 'yes' }), kInvalidArgTypeError);

    symlinkSync('/', '/tmp/link-root');
    symlinkSync('/bundle/worker', '/tmp/link-bundle-worker');
    symlinkSync('/dev/null', '/tmp/link-dev-null');

    const fd1 = openSync('/', 'r');
    const fd2 = openSync('/bundle/worker', 'r');
    const fd3 = openSync('/dev/null', 'r');

    checkRootStat(statSync('/'));
    checkRootStat(statSync('/', { bigint: true }), true);
    checkRootStat(lstatSync('/'));
    checkRootStat(lstatSync('/', { bigint: true }), true);
    checkRootStat(fstatSync(fd1));
    checkRootStat(fstatSync(fd1, { bigint: true }), true);

    checkFileStat(statSync('/bundle/worker'));
    checkFileStat(statSync('/bundle/worker', { bigint: true }), true);
    checkFileStat(lstatSync('/bundle/worker'));
    checkFileStat(lstatSync('/bundle/worker', { bigint: true }), true);
    checkFileStat(fstatSync(fd2));

    checkCharacterDeviceStat(statSync('/dev/null'));
    checkCharacterDeviceStat(statSync('/dev/null', { bigint: true }), true);
    checkCharacterDeviceStat(lstatSync('/dev/null'));
    checkCharacterDeviceStat(lstatSync('/dev/null', { bigint: true }), true);
    checkCharacterDeviceStat(fstatSync(fd3));

    // Check that symlinks are followed by default with statSync.
    checkRootStat(statSync('/tmp/link-root'));
    checkRootStat(statSync('/tmp/link-root', { bigint: true }), true);
    checkFileStat(statSync('/tmp/link-bundle-worker'));
    checkFileStat(statSync('/tmp/link-bundle-worker', { bigint: true }), true);
    checkCharacterDeviceStat(statSync('/tmp/link-dev-null'));
    checkCharacterDeviceStat(
      statSync('/tmp/link-dev-null', { bigint: true }),
      true
    );

    // But lstatSync does not follow symlinks.
    checkLinkStat(lstatSync('/tmp/link-root'));
    checkLinkStat(lstatSync('/tmp/link-root', { bigint: true }), true);
    checkLinkStat(lstatSync('/tmp/link-bundle-worker'));
    checkLinkStat(lstatSync('/tmp/link-bundle-worker', { bigint: true }), true);
    checkLinkStat(lstatSync('/tmp/link-dev-null'));
    checkLinkStat(lstatSync('/tmp/link-dev-null', { bigint: true }), true);

    // Non-existent paths and file descriptors
    throws(() => statSync('/non-existent-path'), { code: 'ENOENT' });
    throws(() => lstatSync('/non-existent-path'), { code: 'ENOENT' });
    throws(() => fstatSync(123), { code: 'EBADF' });

    strictEqual(
      statSync('/non-existent-path', { throwIfNoEntry: false }),
      undefined
    );
    strictEqual(
      lstatSync('/non-existent-path', { throwIfNoEntry: false }),
      undefined
    );

    closeSync(fd1);
    closeSync(fd2);
    closeSync(fd3);
  },
};

export const statTest = {
  async test() {
    throws(() => stat(), kInvalidArgTypeError);
    throws(() => stat('/', 'foo'), kInvalidArgTypeError);
    throws(() => stat('/', { bigint: 'yes' }), kInvalidArgTypeError);
    throws(() => stat('/'), kInvalidArgTypeError);
    throws(() => lstat(), kInvalidArgTypeError);
    throws(() => lstat('/', 'foo'), kInvalidArgTypeError);
    throws(() => lstat('/', { bigint: 'yes' }), kInvalidArgTypeError);
    throws(() => lstat('/'), kInvalidArgTypeError);
    throws(() => fstat(), kInvalidArgTypeError);
    throws(() => fstat(123, 'foo'), kInvalidArgTypeError);
    throws(() => fstat(123, { bigint: 'yes' }), kInvalidArgTypeError);
    throws(() => fstat(123), kInvalidArgTypeError);

    symlinkSync('/', '/tmp/link-root');
    symlinkSync('/bundle/worker', '/tmp/link-bundle-worker');
    symlinkSync('/dev/null', '/tmp/link-dev-null');

    const fd1 = openSync('/', 'r');
    const fd2 = openSync('/bundle/worker', 'r');
    const fd3 = openSync('/dev/null', 'r');

    const doStatTest = async (statFn, path, bigint, checkFn) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      statFn(path, { bigint }, (err, stat) => {
        if (err) {
          reject(err);
        } else {
          try {
            checkFn(stat, bigint);
            resolve();
          } catch (e) {
            reject(e);
          }
        }
      });
      await promise;
    };

    const doFailTest = async (statFn, path, code) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      statFn(path, (err) => {
        if (err) {
          strictEqual(err.code, code);
          resolve();
        } else {
          reject(new Error(`Expected error for path: ${path}`));
        }
      });
      await promise;
    };

    await doStatTest(stat, '/', false, checkRootStat);
    await doStatTest(stat, '/', true, checkRootStat);
    await doStatTest(stat, '/bundle/worker', false, checkFileStat);
    await doStatTest(stat, '/bundle/worker', true, checkFileStat);
    await doStatTest(stat, '/dev/null', false, checkCharacterDeviceStat);
    await doStatTest(stat, '/dev/null', true, checkCharacterDeviceStat);

    await doStatTest(stat, '/tmp/link-root', false, checkRootStat);
    await doStatTest(stat, '/tmp/link-root', true, checkRootStat);
    await doStatTest(stat, '/tmp/link-bundle-worker', false, checkFileStat);
    await doStatTest(stat, '/tmp/link-bundle-worker', true, checkFileStat);
    await doStatTest(
      stat,
      '/tmp/link-dev-null',
      false,
      checkCharacterDeviceStat
    );
    await doStatTest(
      stat,
      '/tmp/link-dev-null',
      true,
      checkCharacterDeviceStat
    );

    await doStatTest(lstat, '/', false, checkRootStat);
    await doStatTest(lstat, '/', true, checkRootStat);
    await doStatTest(lstat, '/bundle/worker', false, checkFileStat);
    await doStatTest(lstat, '/bundle/worker', true, checkFileStat);
    await doStatTest(lstat, '/dev/null', false, checkCharacterDeviceStat);
    await doStatTest(lstat, '/dev/null', true, checkCharacterDeviceStat);

    await doStatTest(lstat, '/tmp/link-root', false, checkLinkStat);
    await doStatTest(lstat, '/tmp/link-root', true, checkLinkStat);
    await doStatTest(lstat, '/tmp/link-bundle-worker', false, checkLinkStat);
    await doStatTest(lstat, '/tmp/link-bundle-worker', true, checkLinkStat);
    await doStatTest(lstat, '/tmp/link-dev-null', false, checkLinkStat);
    await doStatTest(lstat, '/tmp/link-dev-null', true, checkLinkStat);

    await doStatTest(fstat, fd1, false, checkRootStat);
    await doStatTest(fstat, fd1, true, checkRootStat);
    await doStatTest(fstat, fd2, false, checkFileStat);
    await doStatTest(fstat, fd2, true, checkFileStat);
    await doStatTest(fstat, fd3, false, checkCharacterDeviceStat);
    await doStatTest(fstat, fd3, true, checkCharacterDeviceStat);

    // // Non-existent paths and file descriptors
    await doFailTest(stat, '/non-existent-path', 'ENOENT');
    await doFailTest(lstat, '/non-existent-path', 'ENOENT');
    await doFailTest(fstat, 123, 'EBADF');

    closeSync(fd1);
    closeSync(fd2);
    closeSync(fd3);
  },
};

export const statPromisesTest = {
  async test() {
    await rejects(promises.stat(), kInvalidArgTypeError);
    await rejects(promises.stat('/', 'foo'), kInvalidArgTypeError);
    await rejects(promises.stat('/', { bigint: 'yes' }), kInvalidArgTypeError);
    await rejects(promises.lstat(), kInvalidArgTypeError);
    await rejects(promises.lstat('/', 'foo'), kInvalidArgTypeError);
    await rejects(promises.lstat('/', { bigint: 'yes' }), kInvalidArgTypeError);

    symlinkSync('/', '/tmp/link-root');
    symlinkSync('/bundle/worker', '/tmp/link-bundle-worker');
    symlinkSync('/dev/null', '/tmp/link-dev-null');

    await checkRootStat(await promises.stat('/'));
    await checkRootStat(await promises.stat('/', { bigint: true }), true);
    await checkRootStat(await promises.lstat('/'));
    await checkRootStat(await promises.lstat('/', { bigint: true }), true);

    await checkFileStat(await promises.stat('/bundle/worker'));
    await checkFileStat(
      await promises.stat('/bundle/worker', { bigint: true }),
      true
    );
    await checkFileStat(await promises.lstat('/bundle/worker'));
    await checkFileStat(
      await promises.lstat('/bundle/worker', { bigint: true }),
      true
    );

    await checkCharacterDeviceStat(await promises.stat('/dev/null'));
    await checkCharacterDeviceStat(
      await promises.stat('/dev/null', { bigint: true }),
      true
    );
    await checkCharacterDeviceStat(await promises.lstat('/dev/null'));
    await checkCharacterDeviceStat(
      await promises.lstat('/dev/null', { bigint: true }),
      true
    );

    await checkRootStat(await promises.stat('/tmp/link-root'));
    await checkRootStat(
      await promises.stat('/tmp/link-root', { bigint: true }),
      true
    );
    await checkFileStat(await promises.stat('/tmp/link-bundle-worker'));
    await checkFileStat(
      await promises.stat('/tmp/link-bundle-worker', { bigint: true }),
      true
    );
    await checkCharacterDeviceStat(await promises.stat('/tmp/link-dev-null'));
    await checkCharacterDeviceStat(
      await promises.stat('/tmp/link-dev-null', { bigint: true }),
      true
    );

    await checkLinkStat(await promises.lstat('/tmp/link-root'));
    await checkLinkStat(
      await promises.lstat('/tmp/link-root', { bigint: true }),
      true
    );
    await checkLinkStat(await promises.lstat('/tmp/link-bundle-worker'));
    await checkLinkStat(
      await promises.lstat('/tmp/link-bundle-worker', { bigint: true }),
      true
    );
    await checkLinkStat(await promises.lstat('/tmp/link-dev-null'));
    await checkLinkStat(
      await promises.lstat('/tmp/link-dev-null', { bigint: true }),
      true
    );
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
