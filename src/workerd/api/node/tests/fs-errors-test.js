// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import fs from 'node:fs';
import process from 'node:process';

// Checks that node:fs errors carry code, errno, syscall, path and dest the
// way Node.js does, across the sync, callback and promises APIs.

const B = '/tmp/fs-errors';
const M = `${B}/missing`;
// A missing directory whose name the file: URL form would percent-encode.
const U = `${B}/a b%20\u00e5"#? `;

const cases = [
  ['unlink', [M], 'ENOENT', M],
  ['rmdir', [M], 'ENOENT', M],
  ['mkdir', [`${M}/a`], 'ENOENT', `${M}/a`],
  ['readFile', [M], 'ENOENT', M],
  ['readFile', [`${U}/x`], 'ENOENT', `${U}/x`],
  ['writeFile', [`${U}/x`, 'x'], 'ENOENT', `${U}/x`],
  ['unlink', [`${U}/x`], 'ENOENT', `${U}/x`],
  ['stat', [`${U}/x`], 'ENOENT', `${U}/x`],
  ['open', [`${U}/x`], 'ENOENT', `${U}/x`],
  ['mkdir', [`${U}/x`], 'ENOENT', `${U}/x`],
  ['rename', [`${U}/x`, `${B}/x`], 'ENOENT', `${U}/x`, `${B}/x`],
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

// Names reach the file system as written: percent signs and other characters
// the file: URL form encodes are kept, and paths that differ only in encoding
// are different files.
export const fsNamesAsWritten = {
  test() {
    const D = '/tmp/fs-names';
    fs.mkdirSync(D);
    const names = [
      'sp ace \u00e5',
      'p%20q',
      'p q',
      '%41',
      'A',
      'h#a?sh',
      'b\\s',
      'foo ',
      'foo',
      ' lead',
      't\tab',
    ];
    for (const name of names) fs.writeFileSync(`${D}/${name}`, name);
    deepStrictEqual(fs.readdirSync(D).sort(), [...names].sort());
    for (const name of names) {
      strictEqual(fs.readFileSync(`${D}/${name}`, 'utf8'), name);
    }
    fs.mkdirSync(`${D}/sub dir`);
    deepStrictEqual(fs.readdirSync(`${D}/sub dir`), []);

    // A file: URL is percent-decoded, as with Node's fileURLToPath().
    strictEqual(
      fs.readFileSync(new URL(`file://${D}/sp%20ace%20%C3%A5`), 'utf8'),
      'sp ace \u00e5'
    );
    strictEqual(
      fs.readFileSync(new URL(`file://${D}/p%2520q`), 'utf8'),
      'p%20q'
    );
    for (const [bad, code] of [
      ['%2F', 'ERR_INVALID_FILE_URL_PATH'],
      ['%2f', 'ERR_INVALID_FILE_URL_PATH'],
      ['%00', 'ERR_INVALID_ARG_VALUE'],
    ]) {
      throws(() => fs.readFileSync(new URL(`file://${D}/a${bad}b`)), { code });
    }

    // Symbolic links keep the target's name.
    fs.symlinkSync(`${D}/sp ace \u00e5`, `${D}/link`);
    strictEqual(fs.readFileSync(`${D}/link`, 'utf8'), 'sp ace \u00e5');
    strictEqual(fs.readlinkSync(`${D}/link`), `${D}/sp ace \u00e5`);
    fs.renameSync(`${D}/p q`, `${D}/r s`);
    strictEqual(fs.readFileSync(`${D}/r s`, 'utf8'), 'p q');
    strictEqual(fs.existsSync(`${D}/p%20q`), true);

    // Relative paths resolve against a working directory with these names.
    // process.chdir() needs a newer compatibility date.
    if (typeof process.chdir === 'function') {
      const cwd = process.cwd();
      fs.mkdirSync(`${D}/c#d%20`);
      process.chdir(`${D}/c#d%20`);
      try {
        fs.writeFileSync('rel', 'r');
        strictEqual(fs.readFileSync(`${D}/c#d%20/rel`, 'utf8'), 'r');
        fs.writeFileSync(' lead', 'l');
        strictEqual(fs.readFileSync(`${D}/c#d%20/ lead`, 'utf8'), 'l');
      } finally {
        process.chdir(cwd);
      }
    }

    // A URL with another scheme is still rejected with a TypeError.
    throws(() => fs.readFileSync(new URL('https://example.com/x')), TypeError);

    const tmp = fs.mkdtempSync(`${D}/t%`);
    strictEqual(tmp.startsWith(`${D}/t%`), true, tmp);
    strictEqual(fs.statSync(tmp).isDirectory(), true);
  },
};
