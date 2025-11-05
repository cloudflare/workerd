// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ok, match, rejects, strictEqual, throws } from 'node:assert';

import {
  openSync,
  existsSync,
  closeSync,
  statSync,
  fstatSync,
  utimesSync,
  futimesSync,
  lutimesSync,
  utimes,
  futimes,
  lutimes,
  promises,
} from 'node:fs';

strictEqual(typeof openSync, 'function');
strictEqual(typeof closeSync, 'function');
strictEqual(typeof statSync, 'function');
strictEqual(typeof utimesSync, 'function');
strictEqual(typeof futimesSync, 'function');
strictEqual(typeof lutimesSync, 'function');
strictEqual(typeof utimes, 'function');
strictEqual(typeof futimes, 'function');
strictEqual(typeof lutimes, 'function');
strictEqual(typeof promises.utimes, 'function');
strictEqual(typeof promises.lutimes, 'function');

const kInvalidArgTypeError = { code: 'ERR_INVALID_ARG_TYPE' };

function checkStat(path, mtimeMsCheck) {
  const bigint = typeof mtimeMsCheck === 'bigint';
  const { atimeMs, mtimeMs, ctimeMs, birthtimeMs } =
    typeof path === 'number'
      ? fstatSync(path, { bigint })
      : statSync(path, { bigint });
  strictEqual(mtimeMs, mtimeMsCheck);
  strictEqual(ctimeMs, mtimeMsCheck);
  strictEqual(atimeMs, bigint ? 0n : 0);
  strictEqual(birthtimeMs, bigint ? 0n : 0);
}

export const utimesTest = {
  async test() {
    throws(() => utimesSync(123), kInvalidArgTypeError);
    throws(() => utimesSync('', {}), kInvalidArgTypeError);
    throws(() => utimesSync('', 0, {}), kInvalidArgTypeError);
    throws(() => lutimesSync(123), kInvalidArgTypeError);
    throws(() => lutimesSync('', {}), kInvalidArgTypeError);
    throws(() => lutimesSync('', 0, {}), kInvalidArgTypeError);
    throws(() => futimesSync(''), kInvalidArgTypeError);
    throws(() => futimesSync(0, {}), kInvalidArgTypeError);
    throws(() => futimesSync(0, 0, {}), kInvalidArgTypeError);
    throws(() => utimes(0), kInvalidArgTypeError);
    throws(() => utimes('', {}), kInvalidArgTypeError);
    throws(() => utimes('', 0, {}), kInvalidArgTypeError);
    throws(() => utimes('', 0, 0), kInvalidArgTypeError);
    throws(() => utimes(''), kInvalidArgTypeError);
    throws(() => lutimes(0), kInvalidArgTypeError);
    throws(() => lutimes('', {}), kInvalidArgTypeError);
    throws(() => lutimes('', 0, {}), kInvalidArgTypeError);
    throws(() => lutimes('', 0, 0), kInvalidArgTypeError);
    throws(() => lutimes(''), kInvalidArgTypeError);
    throws(() => futimes(''), kInvalidArgTypeError);
    throws(() => futimes(0, {}), kInvalidArgTypeError);
    throws(() => futimes(0, 0, {}), kInvalidArgTypeError);
    throws(() => futimes(0, 0, 0), kInvalidArgTypeError);
    throws(() => futimes(0), kInvalidArgTypeError);
    await rejects(promises.utimes(123), kInvalidArgTypeError);
    await rejects(promises.utimes('', {}), kInvalidArgTypeError);
    await rejects(promises.utimes('', 0, {}), kInvalidArgTypeError);
    await rejects(promises.lutimes(123), kInvalidArgTypeError);
    await rejects(promises.lutimes('', {}), kInvalidArgTypeError);
    await rejects(promises.lutimes('', 0, {}), kInvalidArgTypeError);

    throws(
      () => {
        utimesSync('/tmp/test.txt', 1000, new Date('not a valid date'));
      },
      {
        message: /^The value cannot be converted/,
        name: 'TypeError',
      }
    );

    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    checkStat(fd, 0n);

    utimesSync('/tmp/test.txt', 1000, 2000);
    checkStat(fd, 2000n);

    utimesSync('/tmp/test.txt', 1000, new Date(0));
    checkStat(fd, 0);

    utimesSync('/tmp/test.txt', 3000, '1970-01-01T01:00:00.000Z');
    checkStat(fd, 3600000n);

    lutimesSync('/tmp/test.txt', 3000, 4000);
    checkStat(fd, 4000n);

    lutimesSync('/tmp/test.txt', 1000, new Date(0));
    checkStat(fd, 0);

    lutimesSync('/tmp/test.txt', 3000, '1970-01-01T01:00:00.000Z');
    checkStat(fd, 3600000n);

    futimesSync(fd, 5000, 6000);
    checkStat(fd, 6000n);

    futimesSync(fd, 1000, new Date(0));
    checkStat(fd, 0);

    futimesSync(fd, 3000, '1970-01-01T01:00:00.000Z');
    checkStat(fd, 3600000n);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      utimes('/tmp/test.txt', 8000, new Date('not a valid date'), (err) => {
        try {
          ok(err);
          strictEqual(err.name, 'TypeError');
          match(err.message, /The value cannot be converted/);
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      utimes('/tmp/test.txt', 8000, 9000, (err) => {
        if (err) return reject(err);
        try {
          checkStat(fd, 9000n);
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      lutimes('/tmp/test.txt', 8000, 10000, (err) => {
        if (err) return reject(err);
        try {
          checkStat(fd, 10000n);
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      futimes(fd, 7000, 11000, (err) => {
        if (err) return reject(err);
        try {
          checkStat(fd, 11000n);
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    await promises.utimes('/tmp/test.txt', 12000, 13000);
    checkStat(fd, 13000n);
    await promises.lutimes('/tmp/test.txt', 14000, 15000);
    checkStat(fd, 15000n);

    closeSync(fd);
  },
};
