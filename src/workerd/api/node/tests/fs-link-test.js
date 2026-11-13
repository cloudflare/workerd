// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, ok, rejects, strictEqual, throws } from 'node:assert';

import {
  existsSync,
  statSync,
  linkSync,
  lstatSync,
  symlinkSync,
  readlinkSync,
  realpathSync,
  unlinkSync,
  cpSync,
  mkdirSync,
  writeFileSync,
  readFileSync,
  link,
  symlink,
  readlink,
  realpath,
  unlink,
  promises,
} from 'node:fs';
import { Buffer } from 'node:buffer';

strictEqual(typeof existsSync, 'function');
strictEqual(typeof statSync, 'function');
strictEqual(typeof linkSync, 'function');
strictEqual(typeof lstatSync, 'function');
strictEqual(typeof symlinkSync, 'function');
strictEqual(typeof readlinkSync, 'function');
strictEqual(typeof realpathSync, 'function');
strictEqual(typeof unlinkSync, 'function');
strictEqual(typeof link, 'function');
strictEqual(typeof symlink, 'function');
strictEqual(typeof readlink, 'function');
strictEqual(typeof realpath, 'function');
strictEqual(typeof unlink, 'function');
strictEqual(typeof promises.link, 'function');
strictEqual(typeof promises.symlink, 'function');
strictEqual(typeof promises.readlink, 'function');
strictEqual(typeof promises.realpath, 'function');
strictEqual(typeof promises.unlink, 'function');

const src = '/dev/null';
const dst = '/tmp/test-link';

const kInvalidArgTypeError = { code: 'ERR_INVALID_ARG_TYPE' };
const kEInvalError = { code: 'EINVAL' };
const kENoentError = { code: 'ENOENT' };
const kEExistError = { code: 'EEXIST' };
const kEPermError = { code: 'EPERM' };
const kEIsDir = { code: 'EISDIR' };

export const linkAndUnlinkSyncTest = {
  test() {
    throws(() => linkSync(), kInvalidArgTypeError);
    throws(() => linkSync(''), kInvalidArgTypeError);
    throws(() => realpathSync(), kInvalidArgTypeError);
    throws(() => readlinkSync(), kInvalidArgTypeError);
    throws(() => unlinkSync(), kInvalidArgTypeError);

    ok(existsSync(src));
    ok(!existsSync(dst));
    linkSync(src, dst);
    ok(existsSync(dst));

    // These are the same file.
    deepStrictEqual(statSync(dst), statSync(src));
    deepStrictEqual(lstatSync(dst), statSync(src));

    // Because this is a hard link, the realpath is /tmp/a
    strictEqual(realpathSync(dst), dst);

    // And readlinkSync throws
    throws(() => readlinkSync(dst), kEInvalError);

    // Linking from a non-existent file throws.
    throws(() => linkSync('/does/not/exist', '/tmp/abc'), kENoentError);

    // Make sure the destination does not exist after the failed link.
    ok(!existsSync('/tmp/abc'));
    throws(() => linkSync('/dev/foo', '/tmp/abc'), kENoentError);
    ok(!existsSync('/tmp/abc'));

    // Creating a link in a non-existent directory throws.
    throws(() => linkSync('/dev/null', '/tmp/abc/xyz'), kENoentError);

    // Linking when the path already exists throws.
    throws(() => linkSync(src, dst), kEExistError);

    // Linking with an empty file name throws.
    throws(() => linkSync(src, '/foo/'), kEInvalError);

    // Invalid paths throw
    throws(() => linkSync(src, '/dev/null/test'), kEInvalError);
    throws(() => linkSync('/dev/null/test', '/tmp/null/test'), kENoentError);

    // Attempting to link directories throws
    throws(() => linkSync('/tmp', '/tmp/abc'), kEPermError);

    // The destination can be unlinked
    unlinkSync(dst);
    ok(!existsSync(dst));

    // But unlinking does not remove the source.
    ok(existsSync(src));

    // Cannot unlink read-only files
    throws(() => unlinkSync('/bundle/worker'), kEPermError);
    throws(() => unlinkSync('/bundle'), kEIsDir);

    // Make sure that the failed unlink had no effect.
    ok(existsSync('/bundle/worker'));
    ok(existsSync('/bundle'));
  },
};

export const linkAndUnlinkAsyncCallbackTest = {
  async test() {
    const doLinkTest = async (src, dst) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      link(src, dst, (err) => {
        try {
          if (err) return reject(err);
          ok(existsSync(dst));
          deepStrictEqual(statSync(dst), statSync(src));
          deepStrictEqual(lstatSync(dst), statSync(src));
          strictEqual(realpathSync(dst), dst);
          throws(() => readlinkSync(dst), kEInvalError);
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    };

    const doFailedLinkTest = async (src, dst, expectedError) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      link(src, dst, (err) => {
        try {
          strictEqual(err.code, expectedError.code);
          ok(!existsSync(dst));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    };

    const doUnlinkTest = async (dst) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      unlink(dst, (err) => {
        try {
          if (err) return reject(err);
          ok(!existsSync(dst));
          ok(existsSync(src));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    };

    const doFailedUnlinkTest = async (dst, expectedError) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      unlink(dst, (err) => {
        try {
          strictEqual(err.code, expectedError.code);
          ok(existsSync(dst));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    };

    throws(() => link(), kInvalidArgTypeError);
    throws(() => link(''), kInvalidArgTypeError);
    throws(() => link('', ''), kInvalidArgTypeError);
    throws(() => realpath(), kInvalidArgTypeError);
    throws(() => realpath(''), kInvalidArgTypeError);
    throws(() => readlink(), kInvalidArgTypeError);
    throws(() => readlink(''), kInvalidArgTypeError);
    throws(() => unlink(), kInvalidArgTypeError);
    throws(() => unlink(''), kInvalidArgTypeError);

    ok(existsSync(src));
    ok(!existsSync(dst));
    await doLinkTest(src, dst);

    await doFailedLinkTest('/does/not/exist', '/tmp/abc', kENoentError);
    ok(!existsSync('/tmp/abc'));

    await doFailedLinkTest('/dev/foo', '/tmp/abc', kENoentError);
    ok(!existsSync('/tmp/abc'));

    // Creating a link in a non-existent directory throws.
    await doFailedLinkTest('/dev/null', '/tmp/abc/xyz', kENoentError);

    // Invalid paths throw
    await doFailedLinkTest(src, '/dev/null/test', kEInvalError);
    await doFailedLinkTest('/dev/null/test', '/tmp/null/test', kENoentError);

    // Attempting to link directories throws
    await doFailedLinkTest('/tmp', '/tmp/abc', kEPermError);

    await doUnlinkTest(dst);

    await doFailedUnlinkTest('/bundle/worker', kEPermError);
    await doFailedUnlinkTest('/bundle', kEIsDir);
  },
};

export const linkAndLinkPromisesTest = {
  async test() {
    await rejects(promises.link(), kInvalidArgTypeError);
    await rejects(promises.link(''), kInvalidArgTypeError);
    await rejects(promises.realpath(), kInvalidArgTypeError);
    await rejects(promises.readlink(), kInvalidArgTypeError);
    await rejects(promises.unlink(), kInvalidArgTypeError);

    ok(existsSync(src));
    ok(!existsSync(dst));
    await promises.link(src, dst);

    // These are the same file.
    deepStrictEqual(statSync(dst), statSync(src));
    deepStrictEqual(lstatSync(dst), statSync(src));

    // Because this is a hard link, the realpath is /tmp/a
    strictEqual(await promises.realpath(dst), dst);

    // And readlinkSync throws
    await rejects(promises.readlink(dst), kEInvalError);

    await rejects(promises.link('/does/not/exist', '/tmp/abc'), kENoentError);
    ok(!existsSync('/tmp/abc'));

    await rejects(promises.link('/dev/null', '/tmp/abc/xyz'), kENoentError);

    await rejects(promises.link(src, dst), kEExistError);

    await rejects(promises.link(src, '/foo/'), kEInvalError);

    await rejects(promises.link(src, '/dev/null/test'), kEInvalError);

    await rejects(
      promises.link('/dev/null/test', '/tmp/null/test'),
      kENoentError
    );

    await rejects(promises.link('/tmp', '/tmp/abc'), kEPermError);

    await promises.unlink(dst);
    ok(!existsSync(dst));
    // But unlinking does not remove the source.
    ok(existsSync(src));

    await rejects(promises.unlink('/bundle/worker'), kEPermError);
    await rejects(promises.unlink('/bundle'), kEIsDir);
  },
};

export const symlinkAndUnlinkSyncTest = {
  test() {
    throws(() => symlinkSync(), kInvalidArgTypeError);
    throws(() => symlinkSync(''), kInvalidArgTypeError);

    ok(existsSync(src));
    ok(!existsSync(dst));
    symlinkSync(src, dst);
    ok(existsSync(dst));

    deepStrictEqual(statSync(dst), statSync(src));

    // The symlink points to the source.
    strictEqual(readlinkSync(dst), src);

    // The realpath is the destination
    strictEqual(realpathSync(dst), src);

    // And readlinkSync does not throw
    strictEqual(readlinkSync(dst), src);

    // Linking from a non-existent file works
    symlinkSync('/does/not/exist', '/tmp/abc');

    // Exists sync follows the symlink, and show return false.
    ok(!existsSync('/tmp/abc'));

    // But the symlink does exist.
    const linkStat = lstatSync('/tmp/abc');
    ok(linkStat.isSymbolicLink());

    // Creating a link in a non-existent directory throws.
    throws(() => symlinkSync('/dev/null', '/tmp/abc/xyz'), kENoentError);

    // Linking when the path already exists throws.
    throws(() => symlinkSync(src, dst), kEExistError);

    // Linking with an empty file name throws.
    throws(() => symlinkSync(src, '/foo/'), kEInvalError);

    // Invalid paths throw
    throws(() => symlinkSync(src, '/dev/null/test'), kEInvalError);
    throws(() => symlinkSync('/dev/null/test', '/tmp/null/test'), kENoentError);

    // Symlinking directories works.
    symlinkSync('/tmp', '/tmp/linked-dir');
    ok(existsSync('/tmp/linked-dir'));

    // The destination can be unlinked
    unlinkSync(dst);
    ok(!existsSync(dst));

    // But unlinking does not remove the source.
    ok(existsSync(src));
  },
};

export const symlinkAndUnlinkAsyncCallbackTest = {
  async test() {
    throws(() => symlink(), kInvalidArgTypeError);
    throws(() => symlink(''), kInvalidArgTypeError);
    throws(() => symlink('', ''), kInvalidArgTypeError);
    throws(() => realpath(), kInvalidArgTypeError);
    throws(() => realpath(''), kInvalidArgTypeError);
    throws(() => readlink(), kInvalidArgTypeError);
    throws(() => readlink(''), kInvalidArgTypeError);
    throws(() => unlink(), kInvalidArgTypeError);
    throws(() => unlink(''), kInvalidArgTypeError);

    const doSymlinkTest = async (src, dst) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      symlink(src, dst, (err) => {
        if (err) return reject(err);
        try {
          const srcStat = statSync(src, { throwIfNoEntry: false });
          if (srcStat != null) {
            const dstStat = statSync(dst, { throwIfNoEntry: false });
            deepStrictEqual(dstStat, srcStat);
          } else {
            // The source does not exist but the symlink should still.
            const dstStat = lstatSync(dst);
            ok(dstStat.isSymbolicLink());
          }
          strictEqual(readlinkSync(dst), src);
          strictEqual(realpathSync(dst), src);
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    };

    const doFailedSymlinkTest = async (src, dst, expectedError) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      symlink(src, dst, (err) => {
        try {
          strictEqual(err.code, expectedError.code);
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    };

    const doUnlinkTest = async (dst) => {
      const { promise, resolve, reject } = Promise.withResolvers();
      unlink(dst, (err) => {
        try {
          if (err) return reject(err);
          ok(!existsSync(dst));
          ok(existsSync(src));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    };

    ok(existsSync(src));
    ok(!existsSync(dst));
    await doSymlinkTest(src, dst);
    await doSymlinkTest('/does/not/exist', '/tmp/abc');

    // Exists sync follows the symlink, and show return false.
    ok(!existsSync('/tmp/abc'));

    await doFailedSymlinkTest(src, dst, kEExistError);
    await doFailedSymlinkTest(src, '/foo/', kEInvalError);
    await doFailedSymlinkTest(src, '/dev/null/test', kEInvalError);
    await doFailedSymlinkTest('/dev/null/test', '/tmp/null/test', kENoentError);

    // Symlinking directories works.
    await doSymlinkTest('/tmp', '/tmp/linked-dir');

    // The destination can be unlinked
    await doUnlinkTest(dst);
  },
};

export const symlinkAndUnlinkPromisesTest = {
  async test() {
    await rejects(promises.symlink(), kInvalidArgTypeError);
    await rejects(promises.symlink(''), kInvalidArgTypeError);
    await rejects(promises.realpath(), kInvalidArgTypeError);
    await rejects(promises.readlink(), kInvalidArgTypeError);
    await rejects(promises.unlink(), kInvalidArgTypeError);

    ok(existsSync(src));
    ok(!existsSync(dst));
    await promises.symlink(src, dst);

    deepStrictEqual(statSync(dst), statSync(src));

    // The symlink points to the source.
    strictEqual(await promises.readlink(dst), src);

    // The realpath is the destination
    strictEqual(await promises.realpath(dst), src);

    // And readlink does not throw
    strictEqual(await promises.readlink(dst), src);

    // Linking from a non-existent file works
    await promises.symlink('/does/not/exist', '/tmp/abc');

    // Exists sync follows the symlink, and show return false.
    ok(!existsSync('/tmp/abc'));

    // But the symlink does exist.
    const linkStat = lstatSync('/tmp/abc');
    ok(linkStat.isSymbolicLink());

    // Creating a link in a non-existent directory throws.
    await rejects(promises.symlink('/dev/null', '/tmp/abc/xyz'), kENoentError);

    // Linking when the path already exists throws.
    await rejects(promises.symlink(src, dst), kEExistError);

    // Linking with an empty file name throws.
    await rejects(promises.symlink(src, '/foo/'), kEInvalError);

    // Invalid paths throw
    await rejects(promises.symlink(src, '/dev/null/test'), kEInvalError);
    await rejects(
      promises.symlink('/dev/null/test', '/tmp/null/test'),
      kENoentError
    );

    // Symlinking directories works.
    await promises.symlink('/tmp', '/tmp/linked-dir');
    ok(existsSync('/tmp/linked-dir'));

    // The destination can be unlinked
    await promises.unlink(dst);
    ok(!existsSync(dst));

    // But unlinking does not remove the source.
    ok(existsSync(src));
  },
};

export const relativeSymlinkTargetIsKeptAndResolvedFromTheLink = {
  async test() {
    mkdirSync('/tmp/r/sub', { recursive: true });
    writeFileSync('/tmp/r/sub/x.txt', 'x');

    // The target is stored as written and resolves from the link's directory.
    symlinkSync('sub/x.txt', '/tmp/r/link');
    strictEqual(readlinkSync('/tmp/r/link'), 'sub/x.txt');
    strictEqual(readFileSync('/tmp/r/link', 'utf8'), 'x');
    strictEqual(realpathSync('/tmp/r/link'), '/tmp/r/sub/x.txt');

    symlinkSync('../sub/x.txt', '/tmp/r/sub/up');
    strictEqual(readlinkSync('/tmp/r/sub/up'), '../sub/x.txt');
    strictEqual(readFileSync('/tmp/r/sub/up', 'utf8'), 'x');

    // A relative link to a directory can be traversed.
    symlinkSync('sub', '/tmp/r/dirlink');
    strictEqual(readFileSync('/tmp/r/dirlink/x.txt', 'utf8'), 'x');

    // Absolute targets are also kept as written.
    symlinkSync('/tmp/r//sub/../sub/x.txt', '/tmp/r/abs');
    strictEqual(readlinkSync('/tmp/r/abs'), '/tmp/r//sub/../sub/x.txt');
    strictEqual(readFileSync('/tmp/r/abs', 'utf8'), 'x');

    // Buffer targets are kept as written.
    symlinkSync(Buffer.from('sub/x.txt'), '/tmp/r/buf');
    strictEqual(readlinkSync('/tmp/r/buf'), 'sub/x.txt');
    strictEqual(readFileSync('/tmp/r/buf', 'utf8'), 'x');

    // cp writes the absolute target, so the copy still points at the same
    // file from another directory.
    mkdirSync('/tmp/r2');
    cpSync('/tmp/r/link', '/tmp/r2/link', { dereference: false });
    strictEqual(readlinkSync('/tmp/r2/link'), '/tmp/r/sub/x.txt');
    strictEqual(readFileSync('/tmp/r2/link', 'utf8'), 'x');
    cpSync('/tmp/r/abs', '/tmp/r2/abs', { dereference: false });
    strictEqual(readlinkSync('/tmp/r2/abs'), '/tmp/r//sub/../sub/x.txt');

    // The callback form.
    const { promise, resolve, reject } = Promise.withResolvers();
    symlink('sub/x.txt', '/tmp/r/cb', (err) => (err ? reject(err) : resolve()));
    await promise;
    strictEqual(readlinkSync('/tmp/r/cb'), 'sub/x.txt');
    strictEqual(readFileSync('/tmp/r/cb', 'utf8'), 'x');

    // The promises form.
    await promises.symlink('../sub/x.txt', '/tmp/r/sub/p');
    strictEqual(await promises.readlink('/tmp/r/sub/p'), '../sub/x.txt');
    strictEqual(readFileSync('/tmp/r/sub/p', 'utf8'), 'x');
  },
};
