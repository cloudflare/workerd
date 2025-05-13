import {
  deepStrictEqual,
  notDeepStrictEqual,
  ifError,
  ok,
  match,
  strictEqual,
  throws,
} from 'node:assert';

import {
  accessSync,
  existsSync,
  statSync,
  chmodSync,
  chownSync,
  statfsSync,
  lchmodSync,
  lchownSync,
  linkSync,
  lstatSync,
  symlinkSync,
  readlinkSync,
  realpathSync,
  unlinkSync,
  openSync,
  closeSync,
  fstatSync,
  ftruncateSync,
  fsyncSync,
  fdatasyncSync,
  fchmodSync,
  fchownSync,
  futimesSync,
  utimesSync,
  lutimesSync,
  access,
  stat,
  chmod,
  chown,
  constants,
  statfs,
  link,
  symlink,
  readlink,
  realpath,
  unlink,
  close,
  fstat,
  ftruncate,
  fsync,
  fdatasync,
  fchmod,
  fchown,
  futimes,
  utimes,
  lutimes,
} from 'node:fs';

const { F_OK, R_OK, W_OK, X_OK } = constants;

export const accessSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => accessSync(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => accessSync('/', {}), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    // Known accessible paths, default permissions (F_OK)
    accessSync('/');
    accessSync('/bundle');
    accessSync('/bundle/worker');
    accessSync('/dev');
    accessSync('/dev/null');
    accessSync('/dev/zero');
    accessSync('/dev/full');
    accessSync('/dev/random');
    accessSync('/tmp');

    accessSync(Buffer.from('/'));
    accessSync(Buffer.from('/bundle'));
    accessSync(Buffer.from('/bundle/worker'));
    accessSync(Buffer.from('/dev'));
    accessSync(Buffer.from('/dev/null'));
    accessSync(Buffer.from('/dev/zero'));
    accessSync(Buffer.from('/dev/full'));
    accessSync(Buffer.from('/dev/random'));
    accessSync(Buffer.from('/tmp'));

    accessSync(new URL('file:///'));
    accessSync(new URL('file:///bundle'));
    accessSync(new URL('file:///bundle/worker'));
    accessSync(new URL('file:///dev'));
    accessSync(new URL('file:///dev/null'));
    accessSync(new URL('file:///dev/zero'));
    accessSync(new URL('file:///dev/full'));
    accessSync(new URL('file:///dev/random'));
    accessSync(new URL('file:///tmp'));

    accessSync('/', F_OK);
    accessSync('/bundle', F_OK);
    accessSync('/bundle/worker', F_OK);
    accessSync('/dev', F_OK);
    accessSync('/dev/null', F_OK);
    accessSync('/dev/zero', F_OK);
    accessSync('/dev/full', F_OK);
    accessSync('/dev/random', F_OK);
    accessSync('/tmp', F_OK);

    accessSync(Buffer.from('/'), F_OK);
    accessSync(Buffer.from('/bundle'), F_OK);
    accessSync(Buffer.from('/bundle/worker'), F_OK);
    accessSync(Buffer.from('/dev'), F_OK);
    accessSync(Buffer.from('/dev/null'), F_OK);
    accessSync(Buffer.from('/dev/zero'), F_OK);
    accessSync(Buffer.from('/dev/full'), F_OK);
    accessSync(Buffer.from('/dev/random'), F_OK);
    accessSync(Buffer.from('/tmp'), F_OK);

    accessSync(new URL('file:///'), F_OK);
    accessSync(new URL('file:///bundle'), F_OK);
    accessSync(new URL('file:///bundle/worker'), F_OK);
    accessSync(new URL('file:///dev'), F_OK);
    accessSync(new URL('file:///dev/null'), F_OK);
    accessSync(new URL('file:///dev/zero'), F_OK);
    accessSync(new URL('file:///dev/full'), F_OK);
    accessSync(new URL('file:///dev/random'), F_OK);
    accessSync(new URL('file:///tmp'), F_OK);

    // All of the known paths are readable (R_OK)
    accessSync('/', R_OK);
    accessSync('/bundle', R_OK);
    accessSync('/bundle/worker', R_OK);
    accessSync('/dev', R_OK);
    accessSync('/dev/null', R_OK);
    accessSync('/dev/zero', R_OK);
    accessSync('/dev/full', R_OK);
    accessSync('/dev/random', R_OK);
    accessSync('/tmp', R_OK);

    accessSync(Buffer.from('/'), R_OK);
    accessSync(Buffer.from('/bundle'), R_OK);
    accessSync(Buffer.from('/bundle/worker'), R_OK);
    accessSync(Buffer.from('/dev'), R_OK);
    accessSync(Buffer.from('/dev/null'), R_OK);
    accessSync(Buffer.from('/dev/zero'), R_OK);
    accessSync(Buffer.from('/dev/full'), R_OK);
    accessSync(Buffer.from('/dev/random'), R_OK);
    accessSync(Buffer.from('/tmp'), R_OK);

    accessSync(new URL('file:///'), R_OK);
    accessSync(new URL('file:///bundle'), R_OK);
    accessSync(new URL('file:///bundle/worker'), R_OK);
    accessSync(new URL('file:///dev'), R_OK);
    accessSync(new URL('file:///dev/null'), R_OK);
    accessSync(new URL('file:///dev/zero'), R_OK);
    accessSync(new URL('file:///dev/full'), R_OK);
    accessSync(new URL('file:///dev/random'), R_OK);
    accessSync(new URL('file:///tmp'), R_OK);

    // Only some of the known paths are writable (W_OK)
    throws(() => accessSync('/', W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/bundle', W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/bundle/worker', W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev', W_OK), {
      message: /access denied/,
    });
    accessSync('/dev/null', W_OK);
    accessSync('/dev/zero', W_OK);
    accessSync('/dev/full', W_OK);
    accessSync('/dev/random', W_OK);
    accessSync('/tmp', W_OK);

    // No known paths are executable (X_OK)
    throws(() => accessSync('/', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/bundle', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/bundle/worker', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/null', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/zero', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/full', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/random', X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/tmp', X_OK), {
      message: /access denied/,
    });

    // Paths that don't exist have no permissions.
    throws(() => accessSync('/does/not/exist'), {
      message: /access denied/,
    });
    throws(() => accessSync('/does/not/exist', F_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/does/not/exist', R_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/does/not/exist', W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/does/not/exist', X_OK), {
      message: /access denied/,
    });
  },
};

export const existsSyncTest = {
  test() {
    // Incorrect inputs types results in false returned
    ok(!existsSync(123));

    // Known accessible paths
    ok(existsSync('/'));
    ok(existsSync('/bundle'));
    ok(existsSync('/bundle/worker'));
    ok(existsSync('/dev'));
    ok(existsSync('/dev/null'));
    ok(existsSync('/dev/zero'));
    ok(existsSync('/dev/full'));
    ok(existsSync('/dev/random'));
    ok(existsSync('/tmp'));

    ok(existsSync(Buffer.from('/')));
    ok(existsSync(Buffer.from('/bundle')));
    ok(existsSync(Buffer.from('/bundle/worker')));
    ok(existsSync(Buffer.from('/dev')));
    ok(existsSync(Buffer.from('/dev/null')));
    ok(existsSync(Buffer.from('/dev/zero')));
    ok(existsSync(Buffer.from('/dev/full')));
    ok(existsSync(Buffer.from('/dev/random')));
    ok(existsSync(Buffer.from('/tmp')));

    ok(existsSync(new URL('file:///')));
    ok(existsSync(new URL('file:///bundle')));
    ok(existsSync(new URL('file:///bundle/worker')));
    ok(existsSync(new URL('file:///dev')));
    ok(existsSync(new URL('file:///dev/null')));
    ok(existsSync(new URL('file:///dev/zero')));
    ok(existsSync(new URL('file:///dev/full')));
    ok(existsSync(new URL('file:///dev/random')));
    ok(existsSync(new URL('file:///tmp')));

    // Paths that don't exist
    ok(!existsSync('/does/not/exist'));
  },
};

export const statSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => statSync(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => statSync('/', { bigint: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => statSync('/', { throwIfNoEntry: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    // Known accessible paths
    const stat = statSync('/');
    ok(stat);
    ok(stat.isDirectory());
    ok(!stat.isFile());
    ok(!stat.isBlockDevice());
    ok(!stat.isCharacterDevice());
    ok(!stat.isFIFO());
    ok(!stat.isSocket());
    ok(!stat.isSymbolicLink());

    ok(statSync(Buffer.from('/')));
    ok(statSync(Buffer.from('/')).isDirectory());

    const bundleStat = statSync('/bundle');
    ok(bundleStat);
    ok(bundleStat.isDirectory());
    ok(!bundleStat.isFile());
    ok(!bundleStat.isBlockDevice());
    ok(!bundleStat.isCharacterDevice());
    ok(!bundleStat.isFIFO());
    ok(!bundleStat.isSocket());
    ok(!bundleStat.isSymbolicLink());

    const workerStat = statSync('/bundle/worker');
    ok(workerStat);
    ok(!workerStat.isDirectory());
    ok(workerStat.isFile());
    ok(!workerStat.isBlockDevice());
    ok(!workerStat.isCharacterDevice());
    ok(!workerStat.isFIFO());
    ok(!workerStat.isSocket());
    ok(!workerStat.isSymbolicLink());

    const devStat = statSync('/dev/null');
    ok(devStat);
    ok(!devStat.isDirectory());
    ok(!devStat.isFile());
    ok(!devStat.isBlockDevice());
    ok(devStat.isCharacterDevice());
    ok(!devStat.isFIFO());
    ok(!devStat.isSocket());
    ok(!devStat.isSymbolicLink());

    strictEqual(bundleStat.dev, 0);
    strictEqual(devStat.dev, 1);

    const devStatBigInt = statSync('/dev/null', { bigint: true });
    ok(devStatBigInt);
    strictEqual(typeof devStatBigInt.dev, 'bigint');
    strictEqual(devStatBigInt.dev, 1n);

    // Paths that don't exist throw by default
    throws(() => statSync('/does/not/exist'), {
      message: /file not found/,
    });

    // Unless the `throwIfNoEntry` option is set to false, in which case it
    // returns undefined.
    const statNoThrow = statSync('/does/not/exist', { throwIfNoEntry: false });
    strictEqual(statNoThrow, undefined);
  },
};

export const chmodSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => chmodSync(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => chmodSync('/', {}), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    // Should be non-op
    accessSync('/tmp', W_OK | R_OK);
    chmodSync('/tmp', 0o000);
    chmodSync('/tmp', '000');
    accessSync('/tmp', W_OK | R_OK);

    chmodSync(Buffer.from('/tmp'), 0o000);
    chmodSync(Buffer.from('/tmp'), '000');
    accessSync(Buffer.from('/tmp'), W_OK | R_OK);

    chmodSync(new URL('file:///tmp'), 0o000);
    chmodSync(new URL('file:///tmp'), '000');
    accessSync(new URL('file:///tmp'), W_OK | R_OK);

    // Should throw if the mode is invalid
    throws(() => chmodSync('/tmp', -1), {
      code: /ERR_OUT_OF_RANGE/,
    });
  },
};

export const chownSyncTest = {
  test() {
    // Incorrect input types should throw.
    throws(() => chownSync(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => chownSync('/', {}), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    // Should be non-op
    const stat1 = statSync('/tmp');
    strictEqual(stat1.uid, 0);
    strictEqual(stat1.gid, 0);
    chownSync('/tmp', 1000, 1000);
    const stat2 = statSync('/tmp');
    strictEqual(stat1.uid, stat2.uid);
    strictEqual(stat1.gid, stat2.gid);

    chownSync(Buffer.from('/tmp'), 1000, 1000);
    const stat3 = statSync(Buffer.from('/tmp'));
    strictEqual(stat1.uid, stat3.uid);
    strictEqual(stat1.gid, stat3.gid);

    chownSync(new URL('file:///tmp'), 1000, 1000);
    const stat4 = statSync(new URL('file:///tmp'));
    strictEqual(stat1.uid, stat4.uid);
    strictEqual(stat1.gid, stat4.gid);

    throws(() => chownSync('/tmp', -1000, 0), {
      code: /ERR_OUT_OF_RANGE/,
    });
    throws(() => chownSync('/tmp', 0, -1000), {
      code: /ERR_OUT_OF_RANGE/,
    });
  },
};

export const accessTest = {
  async test() {
    // Incorrect input types should throw.
    throws(() => accessSync(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => accessSync('/', {}), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    const { promise: successDone, resolve: successResolve } =
      Promise.withResolvers();
    const { promise: failDone, resolve: failResolve } = Promise.withResolvers();

    let successCount = 0;
    let failCount = 0;

    const success = (err) => {
      ifError(err);

      // Resolve only after all the expected calls have been made.
      if (++successCount == 86) successResolve();
    };

    const fails = (message) => {
      return (err) => {
        ok(err);
        match(err.message, message);

        if (++failCount == 18) failResolve();
      };
    };

    // Known accessible paths, default permissions (F_OK)
    access('/', success);
    access('/bundle', success);
    access('/bundle/worker', success);
    access('/dev', success);
    access('/dev/null', success);
    access('/dev/zero', success);
    access('/dev/full', success);
    access('/dev/random', success);
    access('/tmp', success);

    access(Buffer.from('/'), success);
    access(Buffer.from('/bundle'), success);
    access(Buffer.from('/bundle/worker'), success);
    access(Buffer.from('/dev'), success);
    access(Buffer.from('/dev/null'), success);
    access(Buffer.from('/dev/zero'), success);
    access(Buffer.from('/dev/full'), success);
    access(Buffer.from('/dev/random'), success);
    access(Buffer.from('/tmp'), success);

    access(new URL('file:///'), success);
    access(new URL('file:///bundle'), success);
    access(new URL('file:///bundle/worker'), success);
    access(new URL('file:///dev'), success);
    access(new URL('file:///dev/null'), success);
    access(new URL('file:///dev/zero'), success);
    access(new URL('file:///dev/full'), success);
    access(new URL('file:///dev/random'), success);
    access(new URL('file:///tmp'), success);

    access('/bundle', F_OK, success);
    access('/', F_OK, success);
    access('/bundle/worker', F_OK, success);
    access('/dev', F_OK, success);
    access('/dev/null', F_OK, success);
    access('/dev/zero', F_OK, success);
    access('/dev/full', F_OK, success);
    access('/dev/random', F_OK, success);
    access('/tmp', F_OK, success);

    access(Buffer.from('/'), F_OK, success);
    access(Buffer.from('/bundle'), F_OK, success);
    access(Buffer.from('/bundle/worker'), F_OK, success);
    access(Buffer.from('/dev'), F_OK, success);
    access(Buffer.from('/dev/null'), F_OK, success);
    access(Buffer.from('/dev/zero'), F_OK, success);
    access(Buffer.from('/dev/full'), F_OK, success);
    access(Buffer.from('/dev/random'), F_OK, success);
    access(Buffer.from('/tmp'), F_OK, success);

    access(new URL('file:///'), F_OK, success);
    access(new URL('file:///bundle'), F_OK, success);
    access(new URL('file:///bundle/worker'), F_OK, success);
    access(new URL('file:///dev'), F_OK, success);
    access(new URL('file:///dev/null'), F_OK, success);
    access(new URL('file:///dev/zero'), F_OK, success);
    access(new URL('file:///dev/full'), F_OK, success);
    access(new URL('file:///dev/random'), F_OK, success);
    access(new URL('file:///tmp'), F_OK, success);

    // All of the known paths are readable (R_OK)
    access('/', R_OK, success);
    access('/bundle', R_OK, success);
    access('/bundle/worker', R_OK, success);
    access('/dev', R_OK, success);
    access('/dev/null', R_OK, success);
    access('/dev/zero', R_OK, success);
    access('/dev/full', R_OK, success);
    access('/dev/random', R_OK, success);
    access('/tmp', R_OK, success);

    access(Buffer.from('/'), R_OK, success);
    access(Buffer.from('/bundle'), R_OK, success);
    access(Buffer.from('/bundle/worker'), R_OK, success);
    access(Buffer.from('/dev'), R_OK, success);
    access(Buffer.from('/dev/null'), R_OK, success);
    access(Buffer.from('/dev/zero'), R_OK, success);
    access(Buffer.from('/dev/full'), R_OK, success);
    access(Buffer.from('/dev/random'), R_OK, success);
    access(Buffer.from('/tmp'), R_OK, success);

    access(new URL('file:///'), R_OK, success);
    access(new URL('file:///bundle'), R_OK, success);
    access(new URL('file:///bundle/worker'), R_OK, success);
    access(new URL('file:///dev'), R_OK, success);
    access(new URL('file:///dev/null'), R_OK, success);
    access(new URL('file:///dev/zero'), R_OK, success);
    access(new URL('file:///dev/full'), R_OK, success);
    access(new URL('file:///dev/random'), R_OK, success);
    access(new URL('file:///tmp'), R_OK, success);

    // Only some of the known paths are writable (W_OK)
    access('/', W_OK, fails(/access denied/));
    access('/bundle', W_OK, fails(/access denied/));
    access('/bundle/worker', W_OK, fails(/access denied/));
    access('/dev', W_OK, fails(/access denied/));

    access('/dev/null', W_OK, success);
    access('/dev/zero', W_OK, success);
    access('/dev/full', W_OK, success);
    access('/dev/random', W_OK, success);
    access('/tmp', W_OK, success);

    // No known paths are executable (X_OK)
    access('/', X_OK, fails(/access denied/));
    access('/bundle', X_OK, fails(/access denied/));
    access('/bundle/worker', X_OK, fails(/access denied/));
    access('/dev', X_OK, fails(/access denied/));
    access('/dev/null', X_OK, fails(/access denied/));
    access('/dev/zero', X_OK, fails(/access denied/));
    access('/dev/full', X_OK, fails(/access denied/));
    access('/dev/random', X_OK, fails(/access denied/));
    access('/tmp', X_OK, fails(/access denied/));

    // Paths that don't exist have no permissions.
    access('/does/not/exist', fails(/access denied/));
    access('/does/not/exist', F_OK, fails(/access denied/));
    access('/does/not/exist', R_OK, fails(/access denied/));
    access('/does/not/exist', W_OK, fails(/access denied/));
    access('/does/not/exist', X_OK, fails(/access denied/));

    await Promise.all([successDone, failDone]);
  },
};

export const statTest = {
  async test() {
    // Incorrect input types should throw.
    throws(() => stat(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => stat('/', { bigint: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => stat('/', { throwIfNoEntry: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    // Known accessible paths
    async function callStatSuccess(path, fn, bigint = false) {
      const { promise, resolve, reject } = Promise.withResolvers();
      stat(path, { bigint }, (err, stat) => {
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

    async function callStatFail(path, message) {
      const { promise, resolve, reject } = Promise.withResolvers();
      stat(path, (err) => {
        if (err) {
          match(err.message, message);
          return resolve();
        }
        reject(new Error('Expected error was not thrown'));
      });
      await promise;
    }

    await callStatSuccess('/', (stat) => {
      ok(stat);
      ok(stat.isDirectory());
      ok(!stat.isFile());
      ok(!stat.isBlockDevice());
      ok(!stat.isCharacterDevice());
      ok(!stat.isFIFO());
      ok(!stat.isSocket());
      ok(!stat.isSymbolicLink());

      strictEqual(stat.dev, 0);
      strictEqual(typeof stat.dev, 'number');
    });

    await callStatSuccess(
      '/',
      (stat) => {
        ok(stat);
        ok(stat.isDirectory());
        ok(!stat.isFile());
        ok(!stat.isBlockDevice());
        ok(!stat.isCharacterDevice());
        ok(!stat.isFIFO());
        ok(!stat.isSocket());
        ok(!stat.isSymbolicLink());

        strictEqual(stat.dev, 0n);
        strictEqual(typeof stat.dev, 'bigint');
      },
      true /* bigint */
    );

    await callStatSuccess(Buffer.from('/'), (stat) => {
      ok(stat);
      ok(stat.isDirectory());
      ok(!stat.isFile());
      ok(!stat.isBlockDevice());
      ok(!stat.isCharacterDevice());
      ok(!stat.isFIFO());
      ok(!stat.isSocket());
      ok(!stat.isSymbolicLink());
    });

    await callStatSuccess(new URL('file:///'), (stat) => {
      ok(stat);
      ok(stat.isDirectory());
      ok(!stat.isFile());
      ok(!stat.isBlockDevice());
      ok(!stat.isCharacterDevice());
      ok(!stat.isFIFO());
      ok(!stat.isSocket());
      ok(!stat.isSymbolicLink());
    });

    await callStatFail('/does/not/exist', /file not found/);
  },
};

export const chmodTest = {
  async test() {
    // Incorrect input types should throw.
    throws(() => chmod(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => chmod('/', {}), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    async function callChmod(path) {
      const { promise, resolve, reject } = Promise.withResolvers();
      chmod(path, 0o000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    // Should be non-op
    accessSync('/tmp', W_OK | R_OK);
    await callChmod('/tmp');
    await callChmod('/tmp');
    accessSync('/tmp', W_OK | R_OK);

    await callChmod(Buffer.from('/tmp'));
    await callChmod(Buffer.from('/tmp'));
    accessSync(Buffer.from('/tmp'), W_OK | R_OK);
  },
};

export const chownTest = {
  async test() {
    // Incorrect input types should throw.
    throws(() => chown(123), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => chown('/', {}), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    async function callChown(path) {
      const { promise, resolve, reject } = Promise.withResolvers();
      chown(path, 1000, 1000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    // Should be non-op
    const stat1 = statSync('/tmp');
    strictEqual(stat1.uid, 0);
    strictEqual(stat1.gid, 0);
    await callChown('/tmp');
    const stat2 = statSync('/tmp');
    strictEqual(stat1.uid, stat2.uid);
    strictEqual(stat1.gid, stat2.gid);

    await callChown(Buffer.from('/tmp'));
    const stat3 = statSync(Buffer.from('/tmp'));
    strictEqual(stat1.uid, stat3.uid);
    strictEqual(stat1.gid, stat3.gid);

    await callChown(new URL('file:///tmp'));
    const stat4 = statSync(new URL('file:///tmp'));
    strictEqual(stat1.uid, stat4.uid);
    strictEqual(stat1.gid, stat4.gid);
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

export const linkTest = {
  async test() {
    ok(!existsSync('/tmp/a'));
    linkSync('/dev/null', '/tmp/a');
    ok(existsSync('/tmp/a'));

    // These are the same file.
    deepStrictEqual(statSync('/tmp/a'), statSync('/dev/null'));
    deepStrictEqual(lstatSync('/tmp/a'), statSync('/dev/null'));

    // Because this is a hard link, the realpath is /tmp/a
    strictEqual(realpathSync('/tmp/a'), '/tmp/a');

    // And readlinkSync throws
    throws(() => readlinkSync('/tmp/a'), {
      message: 'invalid argument',
    });

    ok(!existsSync('/tmp/b'));
    symlinkSync('/dev/null', '/tmp/b');
    ok(existsSync('/tmp/b'));

    deepStrictEqual(statSync('/tmp/b'), statSync('/dev/null'));
    const lstatB = lstatSync('/tmp/b');
    notDeepStrictEqual(lstatB, statSync('/dev/null'));
    ok(lstatB.isSymbolicLink());

    strictEqual(realpathSync('/tmp/b'), '/dev/null');
    strictEqual(readlinkSync('/tmp/b'), '/dev/null');

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      link('/dev/null', '/tmp/c', (err) => {
        try {
          ifError(err);
          ok(existsSync('/tmp/c'));
          deepStrictEqual(statSync('/tmp/c'), statSync('/dev/null'));
          deepStrictEqual(lstatSync('/tmp/c'), statSync('/dev/null'));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      symlink('/dev/null', '/tmp/d', (err) => {
        try {
          ifError(err);
          ok(existsSync('/tmp/d'));
          deepStrictEqual(statSync('/tmp/d'), statSync('/dev/null'));
          notDeepStrictEqual(lstatSync('/tmp/d'), statSync('/dev/null'));
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      readlink('/tmp/d', (err, link) => {
        try {
          ifError(err);
          strictEqual(link, '/dev/null');
        } catch (err) {
          reject(err);
        }
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      realpath('/tmp/d', (err, link) => {
        try {
          ifError(err);
          strictEqual(link, '/dev/null');
        } catch (err) {
          reject(err);
        }
        resolve();
      });
    }

    // Creating a symlink to a file that doesn't exist works.
    symlinkSync('/does/not/exist', '/tmp/e');

    // But creating a hard link to a file that doesn't exist throws.
    throws(() => linkSync('/does/not/exist', '/tmp/f'), {
      message: /file not found/,
    });

    // If the link name is empty, throw
    throws(() => symlinkSync('/does/not/exist', new URL('file:///tmp/g/')), {
      message: /Invalid filename/,
    });

    // If the directory does not exist, throw
    throws(() => symlinkSync('/does/not/exist', new URL('file:///tmp/a/b/c')), {
      message: /Directory does not exist/,
    });

    // If the file already exists, throw
    throws(() => symlinkSync('/does/not/exist', '/tmp/a'), {
      message: /File already exists/,
    });
    throws(() => linkSync('/does/not/exist', '/tmp/a'), {
      message: /File already exists/,
    });

    // If the destination is read-only, throw
    throws(() => symlinkSync('/dev/null', '/bundle/a'), {
      message: /Cannot add a file/,
    });
    throws(() => linkSync('/dev/null', '/bundle/a'), {
      message: /Cannot add a file/,
    });

    // lchmod and lchown are non-ops. They don't throw but they also
    // don't change anything.
    lchmodSync('/tmp/a', 0o000);
    lchmodSync('/tmp/b', 0o000);
    lchownSync('/tmp/a', 1000, 1000);
    lchownSync('/tmp/b', 1000, 1000);

    // unlinkSync removes things
    unlinkSync('/tmp/a');
    ok(!existsSync('/tmp/a'));

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      unlink('/tmp/b', (err) => {
        if (err) return reject(err);
        ok(!existsSync('/tmp/b'));
        resolve();
      });
      await promise;
    }

    // Cannot unlink read-only files
    throws(() => unlinkSync('/bundle/worker'), {
      message: /Cannot remove a file/,
    });

    // Cannot unlink directories
    throws(() => unlinkSync('/bundle'), {
      message: /Cannot unlink a directory/,
    });
  },
};

export const openCloseTest = {
  async test() {
    ok(!existsSync('/tmp/test.txt'));
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    ok(stat);
    ok(stat.isFile());
    ok(!stat.isDirectory());
    strictEqual(stat.size, 0n);

    throws(() => fstatSync(123), {
      message: /Bad file descriptor/,
    });
    throws(() => fstatSync(fd, { bigint: 'yes' }), {
      code: /ERR_INVALID_ARG_TYPE/,
    });
    throws(() => fstatSync('abc'), {
      code: /ERR_INVALID_ARG_TYPE/,
    });

    strictEqual(fd, 0);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fstat(fd, (err, stat) => {
        try {
          ifError(err);
          ok(stat);
          ok(stat.isFile());
          ok(!stat.isDirectory());
          resolve();
        } catch (err) {
          reject(err);
        }
      });
      await promise;
    }

    ftruncateSync(fd, 10);
    const stat2 = fstatSync(fd);
    ok(stat2);
    ok(stat2.isFile());
    strictEqual(stat2.size, 10);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      ftruncate(fd, 20, (err) => {
        if (err) return reject(err);
        const stat3 = fstatSync(fd);
        ok(stat3);
        ok(stat3.isFile());
        strictEqual(stat3.size, 20);
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fsync(fd, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fdatasync(fd, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    // fchmod and fchown are non-ops. They don't throw but they also
    // don't change anything.
    fchmodSync(fd, 0o000);
    fchmodSync(fd, '000');
    fchownSync(fd, 1000, 1000);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fchmod(fd, 0o000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      fchown(fd, 1000, 1000, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }

    fsyncSync(fd);
    fdatasyncSync(fd);

    // Close the file
    closeSync(fd);
    // Can close multiple times
    closeSync(fd);
    // Can close non-existent file descriptors
    closeSync(123);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      close(fd, (err) => {
        if (err) return reject(err);
        resolve();
      });
      await promise;
    }
  },
};

export const utimesTest = {
  async test() {
    const fd = openSync('/tmp/test.txt', 'w+');
    ok(existsSync('/tmp/test.txt'));

    const stat = fstatSync(fd, { bigint: true });
    strictEqual(stat.atimeMs, 0n);
    strictEqual(stat.mtimeMs, 0n);
    strictEqual(stat.ctimeMs, 0n);
    strictEqual(stat.birthtimeMs, 0n);
    strictEqual(stat.atimeNs, 0n);
    strictEqual(stat.mtimeNs, 0n);
    strictEqual(stat.atimeNs, 0n);
    strictEqual(stat.mtimeNs, 0n);

    utimesSync('/tmp/test.txt', 1000, 2000);
    const stat2 = fstatSync(fd, { bigint: true });

    strictEqual(stat2.atimeMs, 0n);
    strictEqual(stat2.mtimeMs, 2000n);

    lutimesSync('/tmp/test.txt', 3000, 4000);
    const stat3 = fstatSync(fd, { bigint: true });
    strictEqual(stat3.atimeMs, 0n);
    strictEqual(stat3.mtimeMs, 4000n);

    futimesSync(fd, 5000, 6000);
    const stat4 = fstatSync(fd, { bigint: true });
    strictEqual(stat4.atimeMs, 0n);
    strictEqual(stat4.mtimeMs, 6000n);

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      futimes(fd, 7000, 8000, (err) => {
        if (err) return reject(err);
        const stat5 = fstatSync(fd, { bigint: true });
        strictEqual(stat5.atimeMs, 0n);
        strictEqual(stat5.mtimeMs, 8000n);
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      utimes('/tmp/test.txt', 8000, 9000, (err) => {
        if (err) return reject(err);
        const stat5 = fstatSync(fd, { bigint: true });
        strictEqual(stat5.atimeMs, 0n);
        strictEqual(stat5.mtimeMs, 9000n);
        resolve();
      });
      await promise;
    }

    {
      const { promise, resolve, reject } = Promise.withResolvers();
      lutimes('/tmp/test.txt', 9000, 10000, (err) => {
        if (err) return reject(err);
        const stat5 = fstatSync(fd, { bigint: true });
        strictEqual(stat5.atimeMs, 0n);
        strictEqual(stat5.mtimeMs, 10000n);
        resolve();
      });
      await promise;
    }

    closeSync(fd);
  },
};
