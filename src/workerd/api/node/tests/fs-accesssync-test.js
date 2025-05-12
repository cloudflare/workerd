import { accessSync, existsSync, constants } from 'node:fs';
import { ok, throws } from 'node:assert';

export const accessSyncTest = {
  test() {
    accessSync('/bundle');
    accessSync('/bundle/worker');
    accessSync('/dev');
    accessSync('/dev/null');
    accessSync('/dev/full');
    accessSync('/dev/random');
    accessSync('/dev/zero');
    accessSync('/tmp');

    accessSync(Buffer.from('/bundle'));
    accessSync(Buffer.from('/bundle/worker'));
    accessSync(Buffer.from('/dev'));
    accessSync(Buffer.from('/dev/null'));
    accessSync(Buffer.from('/dev/full'));
    accessSync(Buffer.from('/dev/random'));
    accessSync(Buffer.from('/dev/zero'));
    accessSync(Buffer.from('/tmp'));

    accessSync(new URL('file:///bundle'));
    accessSync(new URL('file:///bundle/worker'));
    accessSync(new URL('file:///dev'));
    accessSync(new URL('file:///dev/null'));
    accessSync(new URL('file:///dev/full'));
    accessSync(new URL('file:///dev/random'));
    accessSync(new URL('file:///dev/zero'));
    accessSync(new URL('file:///tmp'));

    // Specifying the X_OK flag should always throw an error since files
    // are never executable.
    throws(() => accessSync('/bundle', constants.X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/bundle/worker', constants.X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev', constants.X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/null', constants.X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/full', constants.X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/random', constants.X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/zero', constants.X_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/tmp', constants.X_OK), {
      message: /access denied/,
    });

    // Specifying the W_OK flag should always throw an error when directories
    // and files are not writable.
    throws(() => accessSync('/bundle', constants.W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/bundle/worker', constants.W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev', constants.W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/full', constants.W_OK), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/random', constants.W_OK), {
      message: /access denied/,
    });

    // But should just work if the directory is writable.
    accessSync('/dev/null', constants.W_OK);
    accessSync('/dev/zero', constants.W_OK);
    accessSync('/tmp', constants.W_OK);

    // And R_OK and F_OK should just always work.
    accessSync('/bundle', constants.R_OK);
    accessSync('/bundle/worker', constants.R_OK);
    accessSync('/dev', constants.R_OK);
    accessSync('/dev/null', constants.R_OK);
    accessSync('/dev/full', constants.R_OK);
    accessSync('/dev/random', constants.R_OK);
    accessSync('/dev/zero', constants.R_OK);
    accessSync('/tmp', constants.R_OK);
    accessSync('/bundle', constants.F_OK);
    accessSync('/bundle/worker', constants.F_OK);
    accessSync('/dev', constants.F_OK);
    accessSync('/dev/null', constants.F_OK);
    accessSync('/dev/full', constants.F_OK);
    accessSync('/dev/random', constants.F_OK);
    accessSync('/dev/zero', constants.F_OK);
    accessSync('/tmp', constants.F_OK);

    // Specifying a non-existent file should always throw an error.
    throws(() => accessSync('/bundle/worker/does-not-exist'), {
      message: /access denied/,
    });
    throws(() => accessSync('/dev/nothing'), {
      message: /access denied/,
    });

    // Specifying the wrong input types should throw an error.
    throws(() => accessSync(123), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    throws(() => accessSync({}), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => accessSync('/dev', []), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
  },
};

export const existsSyncTest = {
  test() {
    ok(existsSync('/bundle'));
    ok(existsSync('/bundle/worker'));
    ok(existsSync('/dev'));
    ok(existsSync('/dev/null'));
    ok(existsSync('/dev/full'));
    ok(existsSync('/dev/random'));
    ok(existsSync('/dev/zero'));
    ok(existsSync('/tmp'));

    ok(existsSync(Buffer.from('/bundle')));
    ok(existsSync(Buffer.from('/bundle/worker')));
    ok(existsSync(Buffer.from('/dev')));
    ok(existsSync(Buffer.from('/dev/null')));
    ok(existsSync(Buffer.from('/dev/full')));
    ok(existsSync(Buffer.from('/dev/random')));
    ok(existsSync(Buffer.from('/dev/zero')));
    ok(existsSync(Buffer.from('/tmp')));

    ok(existsSync(new URL('file:///bundle')));
    ok(existsSync(new URL('file:///bundle/worker')));
    ok(existsSync(new URL('file:///dev')));
    ok(existsSync(new URL('file:///dev/null')));
    ok(existsSync(new URL('file:///dev/full')));
    ok(existsSync(new URL('file:///dev/random')));
    ok(existsSync(new URL('file:///dev/zero')));
    ok(existsSync(new URL('file:///tmp')));

    // Specifying a non-existent file should always return false.
    ok(!existsSync('/bundle/worker/does-not-exist'));
  },
};
