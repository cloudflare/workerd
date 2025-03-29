import { throws, strictEqual } from 'node:assert';
import fs from 'node:fs';

export const testSyncMethods = {
  async test() {
    // prettier-ignore
    const syncMethods = [
      'appendFileSync', 'accessSync',    'chownSync',
      'chmodSync',      'closeSync',     'copyFileSync',
      'cpSync',         'existsSync',    'fchownSync',
      'fchmodSync',     'fdatasyncSync', 'fstatSync',
      'fsyncSync',      'ftruncateSync', 'futimesSync',
      'lchownSync',     'lchmodSync',    'linkSync',
      'lstatSync',      'lutimesSync',   'mkdirSync',
      'mkdtempSync',    'openSync',      'readdirSync',
      'readSync',       'readvSync',     'readFileSync',
      'readlinkSync',   'realpathSync',  'renameSync',
      'rmSync',         'rmdirSync',     'statSync',
      'statfsSync',     'symlinkSync',   'truncateSync',
      'unlinkSync',     'utimesSync',    'writeFileSync',
      'writeSync',      'writevSync',    'opendirSync'
    ]

    for (const method of syncMethods) {
      if (method === 'existsSync') {
        strictEqual(fs[method](), false);
        continue;
      }
      throws(
        () => fs[method](),
        { code: 'ERR_INVALID_ARG_TYPE' },
        `Missing exception for method "${method}"`
      );
    }
  },
};
