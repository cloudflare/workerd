// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  deepStrictEqual,
  ok,
  match,
  rejects,
  strictEqual,
  throws,
} from 'node:assert';

import {
  existsSync,
  writeFileSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
  rmdirSync,
  readdirSync,
  mkdtemp,
  mkdir,
  rm,
  rmdir,
  readdir,
  promises,
  opendirSync,
  opendir,
} from 'node:fs';

strictEqual(typeof existsSync, 'function');
strictEqual(typeof writeFileSync, 'function');
strictEqual(typeof mkdirSync, 'function');
strictEqual(typeof mkdtempSync, 'function');
strictEqual(typeof rmSync, 'function');
strictEqual(typeof rmdirSync, 'function');
strictEqual(typeof readdirSync, 'function');
strictEqual(typeof mkdtemp, 'function');
strictEqual(typeof mkdir, 'function');
strictEqual(typeof rm, 'function');
strictEqual(typeof rmdir, 'function');
strictEqual(typeof readdir, 'function');
strictEqual(typeof opendirSync, 'function');
strictEqual(typeof opendir, 'function');
strictEqual(typeof promises.mkdir, 'function');
strictEqual(typeof promises.mkdtemp, 'function');
strictEqual(typeof promises.rm, 'function');
strictEqual(typeof promises.rmdir, 'function');
strictEqual(typeof promises.readdir, 'function');
strictEqual(typeof promises.opendir, 'function');

const kInvalidArgTypeError = { code: 'ERR_INVALID_ARG_TYPE' };
const kInvalidArgValueError = { code: 'ERR_INVALID_ARG_VALUE' };
const kEPermError = { code: 'EPERM' };
const kENoEntError = { code: 'ENOENT' };
const kEExistError = { code: 'EEXIST' };
const kENotDirError = { code: 'ENOTDIR' };
const kENotEmptyError = { code: 'ENOTEMPTY' };

export const mkdirSyncTest = {
  test() {
    throws(() => mkdirSync(), kInvalidArgTypeError);
    throws(() => mkdirSync(123), kInvalidArgTypeError);
    throws(() => mkdirSync('/tmp/testdir', 'hello'), kInvalidArgTypeError);
    throws(
      () => mkdirSync('/tmp/testdir', { recursive: 123 }),
      kInvalidArgTypeError
    );

    // Make a directory.
    ok(!existsSync('/tmp/testdir'));
    strictEqual(mkdirSync('/tmp/testdir'), undefined);
    ok(existsSync('/tmp/testdir'));

    // Making a subdirectory in a non-existing path fails by default
    ok(!existsSync('/tmp/testdir/a/b/c'));
    throws(() => mkdirSync('/tmp/testdir/a/b/c'), kENoEntError);

    // But passing the recursive option allows the entire path to be created.
    ok(!existsSync('/tmp/testdir/a/b/c'));
    strictEqual(
      mkdirSync('/tmp/testdir/a/b/c', { recursive: true }),
      '/tmp/testdir/a'
    );
    ok(existsSync('/tmp/testdir/a/b/c'));

    // Cannot make a directory in a read-only location
    throws(() => mkdirSync('/bundle/a'), kEPermError);

    // Making a directory that already exists is a non-op
    mkdirSync('/tmp/testdir');

    // Attempting to create a directory that already exists as a file throws
    writeFileSync('/tmp/abc', 'Hello World');
    throws(() => mkdirSync('/tmp/abc'), kEExistError);

    // Attempting to create a directory recursively when a parent is a file
    // throws
    throws(() => mkdirSync('/tmp/abc/foo', { recursive: true }), kENotDirError);
  },
};

export const mkdirAsyncCallbackTest = {
  async test() {
    throws(() => mkdir(), kInvalidArgTypeError);
    throws(() => mkdir(123), kInvalidArgTypeError);
    throws(() => mkdir('/tmp/testdir', 'hello'), kInvalidArgTypeError);
    throws(
      () => mkdir('/tmp/testdir', { recursive: 123 }),
      kInvalidArgTypeError
    );

    // Make a directory.
    ok(!existsSync('/tmp/testdir'));
    await new Promise((resolve, reject) => {
      mkdir('/tmp/testdir', (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(existsSync('/tmp/testdir'));

    // Making a subdirectory in a non-existing path fails by default
    ok(!existsSync('/tmp/testdir/a/b/c'));
    await new Promise((resolve, reject) => {
      mkdir('/tmp/testdir/a/b/c', (err) => {
        if (err && err.code === kENoEntError.code) resolve();
        else reject(err);
      });
    });

    // But passing the recursive option allows the entire path to be created.
    ok(!existsSync('/tmp/testdir/a/b/c'));
    await new Promise((resolve, reject) => {
      mkdir('/tmp/testdir/a/b/c', { recursive: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(existsSync('/tmp/testdir/a/b/c'));

    // Cannot make a directory in a read-only location
    await new Promise((resolve, reject) => {
      mkdir('/bundle/a', (err) => {
        if (err && err.code === kEPermError.code) resolve();
        else reject(err);
      });
    });

    // Making a directory that already exists is a non-op
    await new Promise((resolve, reject) => {
      mkdir('/tmp/testdir', (err) => {
        if (err) reject(err);
        else resolve();
      });
    });

    // Attempting to create a directory that already exists as a file throws
    writeFileSync('/tmp/abc', 'Hello World');
    await new Promise((resolve, reject) => {
      mkdir('/tmp/abc', (err) => {
        if (err && err.code === kEExistError.code) resolve();
        else reject(err);
      });
    });

    // Attempting to create a directory recursively when a parent is a file
    // throws
    await new Promise((resolve, reject) => {
      mkdir('/tmp/abc/foo', { recursive: true }, (err) => {
        if (err && err.code === kENotDirError.code) resolve();
        else reject(err);
      });
    });
  },
};

export const mkdirAsyncPromiseTest = {
  async test() {
    await rejects(promises.mkdir(), kInvalidArgTypeError);
    await rejects(promises.mkdir(123), kInvalidArgTypeError);
    await rejects(
      promises.mkdir('/tmp/testdir', 'hello'),
      kInvalidArgTypeError
    );
    await rejects(
      promises.mkdir('/tmp/testdir', { recursive: 123 }),
      kInvalidArgTypeError
    );

    // Make a directory.
    ok(!existsSync('/tmp/testdir'));
    await promises.mkdir('/tmp/testdir');
    ok(existsSync('/tmp/testdir'));

    // Making a subdirectory in a non-existing path fails by default
    ok(!existsSync('/tmp/testdir/a/b/c'));
    await rejects(promises.mkdir('/tmp/testdir/a/b/c'), kENoEntError);

    // But passing the recursive option allows the entire path to be created.
    ok(!existsSync('/tmp/testdir/a/b/c'));
    await promises.mkdir('/tmp/testdir/a/b/c', { recursive: true });
    ok(existsSync('/tmp/testdir/a/b/c'));

    // Cannot make a directory in a read-only location
    await rejects(promises.mkdir('/bundle/a'), kEPermError);

    // Making a directory that already exists is a non-op
    await promises.mkdir('/tmp/testdir');

    // Attempting to create a directory that already exists as a file throws
    writeFileSync('/tmp/abc', 'Hello World');
    await rejects(promises.mkdir('/tmp/abc'), kEExistError);

    // Attempting to create a directory recursively when a parent is a file
    // throws
    await rejects(
      promises.mkdir('/tmp/abc/foo', { recursive: true }),
      kENotDirError
    );
  },
};

export const mkdtempSyncTest = {
  test() {
    throws(() => mkdtempSync(), kInvalidArgTypeError);
    const ret1 = mkdtempSync('/tmp/testdir-');
    const ret2 = mkdtempSync('/tmp/testdir-');
    match(ret1, /\/tmp\/testdir-\d+/);
    match(ret2, /\/tmp\/testdir-\d+/);
    ok(existsSync(ret1));
    ok(existsSync(ret2));
    throws(() => mkdtempSync('/bundle/testdir-'), kEPermError);
  },
};

export const mkdtempAsyncCallbackTest = {
  async test() {
    throws(() => mkdtemp(), kInvalidArgTypeError);
    const ret1 = await new Promise((resolve, reject) => {
      mkdtemp('/tmp/testdir-', (err, dir) => {
        if (err) reject(err);
        else resolve(dir);
      });
    });
    const ret2 = await new Promise((resolve, reject) => {
      mkdtemp('/tmp/testdir-', (err, dir) => {
        if (err) reject(err);
        else resolve(dir);
      });
    });
    match(ret1, /\/tmp\/testdir-\d+/);
    match(ret2, /\/tmp\/testdir-\d+/);
    ok(existsSync(ret1));
    ok(existsSync(ret2));
    await new Promise((resolve, reject) => {
      mkdtemp('/bundle/testdir-', (err) => {
        if (err && err.code === kEPermError.code) resolve();
        else reject(err);
      });
    });
  },
};

export const mkdtempAsyncPromiseTest = {
  async test() {
    await rejects(promises.mkdtemp(), kInvalidArgTypeError);
    const ret1 = await promises.mkdtemp('/tmp/testdir-');
    const ret2 = await promises.mkdtemp('/tmp/testdir-');
    match(ret1, /\/tmp\/testdir-\d+/);
    match(ret2, /\/tmp\/testdir-\d+/);
    ok(existsSync(ret1));
    ok(existsSync(ret2));
    await rejects(promises.mkdtemp('/bundle/testdir-'), kEPermError);
  },
};

export const rmSyncTest = {
  test() {
    // Passing incorrect types for options throws
    throws(
      () => rmSync('/tmp/testdir', { recursive: 'yes' }),
      kInvalidArgTypeError
    );
    throws(() => rmSync('/tmp/testdir', 'abc'), kInvalidArgTypeError);
    throws(
      () => rmSync('/tmp/testdir', { force: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmSync('/tmp/testdir', { maxRetries: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmSync('/tmp/testdir', { retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmSync('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmSync('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }),
      kInvalidArgTypeError
    );
    throws(
      () =>
        rmSync('/tmp/testdir', { maxRetries: 1, retryDelay: 1, force: 'yes' }),
      kInvalidArgTypeError
    );

    throws(
      () => rmdirSync('/tmp/testdir', { recursive: 'yes' }),
      kInvalidArgTypeError
    );
    throws(() => rmdirSync('/tmp/testdir', 'abc'), kInvalidArgTypeError);
    throws(
      () => rmdirSync('/tmp/testdir', { maxRetries: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmdirSync('/tmp/testdir', { retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmdirSync('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmdirSync('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }),
      kInvalidArgTypeError
    );

    ok(!existsSync('/tmp/testdir'));
    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');

    // When the recusive option is not set, then removing a directory
    // with children throws...
    throws(() => rmdirSync('/tmp/testdir'), kENotEmptyError);
    ok(existsSync('/tmp/testdir'));

    // But works when the recursive option is set
    rmdirSync('/tmp/testdir', { recursive: true });
    ok(!existsSync('/tmp/testdir'));

    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');
    writeFileSync('/tmp/testdir/b.txt', 'Hello World');
    ok(existsSync('/tmp/testdir/a.txt'));

    // trying to remove a file with rmdir throws
    throws(() => rmdirSync('/tmp/testdir/a.txt'), kENotDirError);

    // removing a file with rm works
    rmSync('/tmp/testdir/a.txt');
    ok(!existsSync('/tmp/testdir/a.txt'));

    // Calling rmSync when the directory is not empty throws
    throws(() => rmSync('/tmp/testdir'), kENotEmptyError);
    ok(existsSync('/tmp/testdir'));

    // But works when the recursive option is set
    throws(() => rmSync('/tmp/testdir'));
    rmSync('/tmp/testdir', { recursive: true });
    ok(!existsSync('/tmp/testdir'));
  },
};

export const rmAsyncCallbackTest = {
  async test() {
    // Passing incorrect types for options throws
    throws(
      () => rm('/tmp/testdir', { recursive: 'yes' }),
      kInvalidArgTypeError
    );
    throws(() => rm('/tmp/testdir', 'abc'), kInvalidArgTypeError);
    throws(() => rm('/tmp/testdir', { force: 'yes' }), kInvalidArgTypeError);
    throws(
      () => rm('/tmp/testdir', { maxRetries: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rm('/tmp/testdir', { retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rm('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rm('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }),
      kInvalidArgTypeError
    );
    throws(
      () => rm('/tmp/testdir', { maxRetries: 1, retryDelay: 1, force: 'yes' }),
      kInvalidArgTypeError
    );

    throws(
      () => rmdir('/tmp/testdir', { recursive: 'yes' }),
      kInvalidArgTypeError
    );
    throws(() => rmdir('/tmp/testdir', 'abc'), kInvalidArgTypeError);
    throws(
      () => rmdir('/tmp/testdir', { maxRetries: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmdir('/tmp/testdir', { retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmdir('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    throws(
      () => rmdir('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }),
      kInvalidArgTypeError
    );

    ok(!existsSync('/tmp/testdir'));
    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');

    // When the recusive option is not set, then removing a directory
    // with children throws...
    await new Promise((resolve, reject) => {
      rmdir('/tmp/testdir', (err) => {
        if (err && err.code === kENotEmptyError.code) resolve();
        else reject(err);
      });
    });

    ok(existsSync('/tmp/testdir'));
    // But works when the recursive option is set
    await new Promise((resolve, reject) => {
      rmdir('/tmp/testdir', { recursive: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(!existsSync('/tmp/testdir'));
    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');
    writeFileSync('/tmp/testdir/b.txt', 'Hello World');

    ok(existsSync('/tmp/testdir/a.txt'));
    // trying to remove a file with rmdir throws
    await new Promise((resolve, reject) => {
      rmdir('/tmp/testdir/a.txt', (err) => {
        if (err && err.code === kENotDirError.code) resolve();
        else reject(err);
      });
    });
    // removing a file with rm works
    await new Promise((resolve, reject) => {
      rm('/tmp/testdir/a.txt', (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(!existsSync('/tmp/testdir/a.txt'));
    // Calling rm when the directory is not empty throws
    await new Promise((resolve, reject) => {
      rm('/tmp/testdir', (err) => {
        if (err && err.code === kENotEmptyError.code) resolve();
        else reject(err);
      });
    });
    ok(existsSync('/tmp/testdir'));
    // But works when the recursive option is set
    await new Promise((resolve, reject) => {
      rm('/tmp/testdir', { recursive: true }, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
    ok(!existsSync('/tmp/testdir'));
  },
};

export const rmAsyncPromiseTest = {
  async test() {
    // Passing incorrect types for options throws
    await rejects(
      promises.rm('/tmp/testdir', { recursive: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(promises.rm('/tmp/testdir', 'abc'), kInvalidArgTypeError);
    await rejects(
      promises.rm('/tmp/testdir', { force: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rm('/tmp/testdir', { maxRetries: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rm('/tmp/testdir', { retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rm('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rm('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rm('/tmp/testdir', {
        maxRetries: 1,
        retryDelay: 1,
        force: 'yes',
      }),
      kInvalidArgTypeError
    );

    await rejects(
      promises.rmdir('/tmp/testdir', { recursive: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(promises.rmdir('/tmp/testdir', 'abc'), kInvalidArgTypeError);
    await rejects(
      promises.rmdir('/tmp/testdir', { maxRetries: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rmdir('/tmp/testdir', { retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rmdir('/tmp/testdir', { maxRetries: 1, retryDelay: 'yes' }),
      kInvalidArgTypeError
    );
    await rejects(
      promises.rmdir('/tmp/testdir', { maxRetries: 'yes', retryDelay: 1 }),
      kInvalidArgTypeError
    );

    ok(!existsSync('/tmp/testdir'));
    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');

    // When the recusive option is not set, then removing a directory
    // with children throws...
    await rejects(promises.rmdir('/tmp/testdir'), kENotEmptyError);

    ok(existsSync('/tmp/testdir'));
    // But works when the recursive option is set
    await promises.rmdir('/tmp/testdir', { recursive: true });
    ok(!existsSync('/tmp/testdir'));
    mkdirSync('/tmp/testdir');
    writeFileSync('/tmp/testdir/a.txt', 'Hello World');
    writeFileSync('/tmp/testdir/b.txt', 'Hello World');
    ok(existsSync('/tmp/testdir/a.txt'));
    // trying to remove a file with rmdir throws
    await rejects(promises.rmdir('/tmp/testdir/a.txt'), kENotDirError);
    // removing a file with rm works
    await promises.rm('/tmp/testdir/a.txt');
    ok(!existsSync('/tmp/testdir/a.txt'));
    // Calling rm when the directory is not empty throws
    await rejects(promises.rm('/tmp/testdir'), kENotEmptyError);
    ok(existsSync('/tmp/testdir'));
    // But works when the recursive option is set
    await promises.rm('/tmp/testdir', { recursive: true });
    ok(!existsSync('/tmp/testdir'));
  },
};

export const readdirSyncTest = {
  test() {
    throws(() => readdirSync(), kInvalidArgTypeError);
    throws(() => readdirSync(123), kInvalidArgTypeError);
    throws(
      () => readdirSync('/tmp/testdir', { withFileTypes: 123 }),
      kInvalidArgTypeError
    );
    throws(
      () => readdirSync('/tmp/testdir', { recursive: 123 }),
      kInvalidArgTypeError
    );
    throws(
      () =>
        readdirSync('/tmp/testdir', { withFileTypes: true, recursive: 123 }),
      kInvalidArgTypeError
    );

    deepStrictEqual(readdirSync('/'), ['bundle', 'tmp', 'dev']);

    deepStrictEqual(readdirSync('/', 'buffer'), [
      Buffer.from('bundle'),
      Buffer.from('tmp'),
      Buffer.from('dev'),
    ]);

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
      const ents = readdirSync('/', {
        withFileTypes: true,
        encoding: 'buffer',
      });
      strictEqual(ents.length, 3);

      deepStrictEqual(ents[0].name, Buffer.from('bundle'));
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

export const readdirAsyncCallbackTest = {
  async test() {
    deepStrictEqual(
      await new Promise((resolve, reject) => {
        readdir('/', (err, files) => {
          if (err) reject(err);
          else resolve(files);
        });
      }),
      ['bundle', 'tmp', 'dev']
    );

    {
      const ents = await new Promise((resolve, reject) => {
        readdir('/', { withFileTypes: true }, (err, files) => {
          if (err) reject(err);
          else resolve(files);
        });
      });
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
      const ents = await new Promise((resolve, reject) => {
        readdir(
          '/',
          { withFileTypes: true, encoding: 'buffer' },
          (err, files) => {
            if (err) reject(err);
            else resolve(files);
          }
        );
      });
      strictEqual(ents.length, 3);

      deepStrictEqual(ents[0].name, Buffer.from('bundle'));
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
      const ents = await new Promise((resolve, reject) => {
        readdir('/', { withFileTypes: true, recursive: true }, (err, files) => {
          if (err) reject(err);
          else resolve(files);
        });
      });
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

export const readdirAsyncPromiseTest = {
  async test() {
    deepStrictEqual(await promises.readdir('/'), ['bundle', 'tmp', 'dev']);

    {
      const ents = await promises.readdir('/', { withFileTypes: true });
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
      const ents = await promises.readdir('/', {
        withFileTypes: true,
        encoding: 'buffer',
      });
      strictEqual(ents.length, 3);

      deepStrictEqual(ents[0].name, Buffer.from('bundle'));
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
      const ents = await promises.readdir('/', {
        withFileTypes: true,
        recursive: true,
      });
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

export const opendirSyncTest = {
  test() {
    throws(() => opendirSync(), kInvalidArgTypeError);
    throws(() => opendirSync(123), kInvalidArgTypeError);
    throws(() => opendirSync('/tmp', { encoding: 123 }), kInvalidArgValueError);

    const dir = opendirSync('/', { recursive: true });
    strictEqual(dir.path, '/');
    strictEqual(dir.readSync().name, 'bundle');
    strictEqual(dir.readSync().name, 'bundle/worker');
    strictEqual(dir.readSync().name, 'tmp');
    strictEqual(dir.readSync().name, 'dev');
    strictEqual(dir.readSync().name, 'dev/null');
    strictEqual(dir.readSync().name, 'dev/zero');
    strictEqual(dir.readSync().name, 'dev/full');
    strictEqual(dir.readSync().name, 'dev/random');
    strictEqual(dir.readSync(), null); // All done.
    dir.closeSync();

    // Closing again throws
    throws(() => dir.closeSync(), { code: 'ERR_DIR_CLOSED' });
    // Reading again throws
    throws(() => dir.readSync(), { code: 'ERR_DIR_CLOSED' });
  },
};

export const opendirSyncAndAsyncTest = {
  async test() {
    throws(() => opendir(), kInvalidArgTypeError);
    throws(() => opendir(123), kInvalidArgTypeError);
    throws(() => opendir('/tmp', { encoding: 123 }), kInvalidArgValueError);

    const { promise, resolve, reject } = Promise.withResolvers();
    opendir('/', { recursive: true }, (err, dir) => {
      if (err) reject(err);
      else resolve(dir);
    });

    await using dir = await promise;

    strictEqual((await dir.read()).name, 'bundle');
    strictEqual((await dir.read()).name, 'bundle/worker');
    strictEqual((await dir.read()).name, 'tmp');
    strictEqual((await dir.read()).name, 'dev');
    strictEqual((await dir.read()).name, 'dev/null');
    strictEqual((await dir.read()).name, 'dev/zero');
    strictEqual((await dir.read()).name, 'dev/full');
    strictEqual((await dir.read()).name, 'dev/random');
    strictEqual(await dir.read(), null); // All done.
  },
};

export const opendirSyncAndAsyncTest2 = {
  async test() {
    throws(() => opendir(), kInvalidArgTypeError);
    throws(() => opendir(123), kInvalidArgTypeError);
    throws(() => opendir('/tmp', { encoding: 123 }), kInvalidArgValueError);

    const { promise, resolve, reject } = Promise.withResolvers();
    opendir('/', { recursive: true }, (err, dir) => {
      if (err) reject(err);
      else resolve(dir);
    });

    await using dir = await promise;

    const entries = await Array.fromAsync(dir);

    strictEqual(entries.length, 8);
    strictEqual(entries[0].name, 'bundle');
    strictEqual(entries[1].name, 'bundle/worker');
    strictEqual(entries[2].name, 'tmp');
    strictEqual(entries[3].name, 'dev');
    strictEqual(entries[4].name, 'dev/null');
    strictEqual(entries[5].name, 'dev/zero');
    strictEqual(entries[6].name, 'dev/full');
    strictEqual(entries[7].name, 'dev/random');
  },
};
