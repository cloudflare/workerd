// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';
import fs from 'node:fs';

// Checks that node:fs errors carry code, errno, syscall, path and dest the
// way Node.js does, across the sync, callback and promises APIs.

const B = '/tmp/fs-errors';
const M = `${B}/missing`;

const cases = [
  ['unlink', [M], 'ENOENT', M],
  ['rmdir', [M], 'ENOENT', M],
  ['mkdir', [`${M}/a`], 'ENOENT', `${M}/a`],
  ['readFile', [M], 'ENOENT', M],
  ['writeFile', [`${M}/a`, 'x'], 'ENOENT', `${M}/a`],
  ['rename', [M, `${B}/x`], 'ENOENT', M, `${B}/x`],
  ['copyFile', [M, `${B}/x`], 'ENOENT', M, `${B}/x`],
  ['link', [M, `${B}/x`], 'ENOENT', M, `${B}/x`],
  ['writeFile', [`${B}/file`, 'x', { flag: 'wx' }], 'EEXIST', `${B}/file`],
  ['link', [`${B}/file`, `${B}/link`], 'EEXIST', `${B}/file`, `${B}/link`],
  ['symlink', [`${B}/file`, `${B}/link`], 'EEXIST', `${B}/file`, `${B}/link`],
  [
    'copyFile',
    [`${B}/file`, `${B}/link`, fs.constants.COPYFILE_EXCL],
    'EEXIST',
    `${B}/file`,
    `${B}/link`,
  ],
  ['rmdir', [`${B}/file`], 'ENOTDIR', `${B}/file`],
  ['mkdir', [`${B}/file/x`], 'ENOTDIR', `${B}/file/x`],
  ['readFile', [`${B}/dir`], 'EISDIR', `${B}/dir`],
  ['writeFile', [`${B}/dir`, 'x'], 'EISDIR', `${B}/dir`],
  ['rmdir', [`${B}/dir`], 'ENOTEMPTY', `${B}/dir`],
];

function check(err, api, fn, code, path, dest) {
  const what = `${api} ${fn} ${code}`;
  strictEqual(err?.code, code, what);
  strictEqual(typeof err.errno, 'number', what);
  strictEqual(err.errno < 0, true, what);
  strictEqual(typeof err.syscall, 'string', what);
  strictEqual(err.path, path, what);
  strictEqual(err.dest, dest, what);
}

export const fsErrorProperties = {
  async test() {
    fs.mkdirSync(`${B}/dir/sub`, { recursive: true });
    fs.writeFileSync(`${B}/file`, 'x');
    fs.writeFileSync(`${B}/dir/sub/f`, 'x');
    fs.symlinkSync(`${B}/file`, `${B}/link`);
    for (const [fn, args, code, path, dest] of cases) {
      let err;
      try {
        fs[`${fn}Sync`](...args);
      } catch (e) {
        err = e;
      }
      check(err, 'sync', fn, code, path, dest);

      // Some callback APIs throw synchronously instead of calling back.
      err = await new Promise((resolve) => {
        try {
          fs[fn](...args, resolve);
        } catch (e) {
          resolve(e);
        }
      });
      check(err, 'callback', fn, code, path, dest);

      err = await fs.promises[fn](...args).then(
        () => undefined,
        (e) => e
      );
      check(err, 'promises', fn, code, path, dest);
    }
  },
};

function messageOf(fn) {
  try {
    fn();
  } catch (e) {
    return e.message;
  }
  throw new Error('did not throw');
}

export const fsErrorMessages = {
  test() {
    strictEqual(
      messageOf(() => fs.unlinkSync(M)),
      `no such file or directory, unlink '${M}'`
    );
    const rename = messageOf(() => fs.renameSync(M, `${B}/x`));
    strictEqual(rename.endsWith(` '${M}' -> '${B}/x'`), true, rename);
    const link = messageOf(() => fs.linkSync(M, `${B}/x`));
    strictEqual(link.endsWith(` '${M}' -> '${B}/x'`), true, link);
  },
};
