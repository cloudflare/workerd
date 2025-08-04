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
