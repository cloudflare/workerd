// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, throws } from 'node:assert';

// sync and datasync are generally non-ops. The most they do is
// verify that the file descriptor is valid.

import {
  openSync,
  closeSync,
  fsyncSync,
  fdatasyncSync,
  fdatasync,
  fsync,
  statfs,
  statfsSync,
  writeFileSync,
  mkdirSync,
  symlinkSync,
} from 'node:fs';

strictEqual(typeof openSync, 'function');
strictEqual(typeof closeSync, 'function');
strictEqual(typeof fdatasyncSync, 'function');
strictEqual(typeof fsyncSync, 'function');
strictEqual(typeof fdatasync, 'function');
strictEqual(typeof fsync, 'function');

const kInvalidArgTypeError = { code: 'ERR_INVALID_ARG_TYPE' };
const kBadFError = { code: 'EBADF' };

export const miscTest = {
  async test() {
    throws(() => fsyncSync(), kInvalidArgTypeError);
    throws(() => fdatasyncSync(), kInvalidArgTypeError);
    throws(() => fsyncSync('hello'), kInvalidArgTypeError);
    throws(() => fdatasyncSync('hello'), kInvalidArgTypeError);
    throws(() => fsync(), kInvalidArgTypeError);
    throws(() => fdatasync(), kInvalidArgTypeError);
    throws(() => fsync('hello'), kInvalidArgTypeError);
    throws(() => fdatasync('hello'), kInvalidArgTypeError);
    throws(() => fsyncSync(123), kBadFError);
    throws(() => fdatasyncSync(123), kBadFError);

    const fd = openSync('/dev/null', 'r');
    fsyncSync(fd);
    fdatasyncSync(fd);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fsync(fd, (err) => {
        if (err) {
          reject(err);
          return;
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fdatasync(fd, (err) => {
        if (err) {
          reject(err);
          return;
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fsync(123, (err) => {
        if (!err) {
          reject(new Error('Expected an error'));
          return;
        }
        strictEqual(err.code, 'EBADF');
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fdatasync(123, (err) => {
        if (!err) {
          reject(new Error('Expected an error'));
          return;
        }
        strictEqual(err.code, 'EBADF');
        resolve();
      });
      await promise;
    }

    closeSync(fd);
  },
};

export const statFsTest = {
  async test() {
    const stat = statfsSync('/');
    strictEqual(typeof stat, 'object');
    strictEqual(stat.type, 0);
    strictEqual(stat.bsize, 0);
    strictEqual(stat.blocks, 0);
    strictEqual(stat.bfree, 0);
    strictEqual(stat.bavail, 0);
    strictEqual(stat.files, 0);
    strictEqual(stat.ffree, 0);

    throws(() => statfsSync(123), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => statfsSync('/does/not/exist', { bigint: 123 }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    const { promise, resolve, reject } = Promise.withResolvers();
    statfs('/', (err, stat) => {
      if (err) reject(err);
      else {
        strictEqual(typeof stat, 'object');
        strictEqual(stat.type, 0);
        strictEqual(stat.bsize, 0);
        strictEqual(stat.blocks, 0);
        strictEqual(stat.bfree, 0);
        strictEqual(stat.bavail, 0);
        strictEqual(stat.files, 0);
        strictEqual(stat.ffree, 0);
        resolve();
      }
    });
    await promise;

    throws(() => statfs(123, () => {}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => statfs('/does/not/exist', { bigint: 123 }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
  },
};

export const pathLimitTest = {
  test() {
    // Trying to open a path longer than 4096 characters should throw an error.
    const longPath = '/tmp/a'.repeat(4097);
    throws(() => openSync(longPath, 'r'), {
      message: /File path is too long/,
    });

    // Trying to open a path with more than 48 segments should throw an error.
    const tooManySegments = '/a'.repeat(49);
    throws(() => openSync(tooManySegments, 'r'), {
      message: /File path has too many segments/,
    });
  },
};

export const dirEntryLimitTest = {
  test() {
    for (let i = 0; i < 1024; i++) {
      writeFileSync(`/tmp/${i}`, `${i}`);
    }

    // It is not permitted to create a directory with more than 1024 entries.
    throws(() => writeFileSync('/tmp/1024', '1024'), {
      message: /Directory entry count exceeded/,
    });
    throws(() => mkdirSync('/tmp/1024'), {
      message: /Directory entry count exceeded/,
    });
    throws(() => symlinkSync('/tmp/0', '/tmp/1024'), {
      message: /Directory entry count exceeded/,
    });
  },
};
