// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as utilMod from 'node:util';
import utilDefault from 'node:util';
import * as sysMod from 'node:sys';
import sysDefault from 'node:sys';

export const utilSetTraceSigIntTest = {
  test() {
    assert.strictEqual(typeof utilMod.setTraceSigInt, 'function');
    assert.strictEqual(typeof utilDefault.setTraceSigInt, 'function');
    // Aliased as sys too.
    assert.strictEqual(typeof sysMod.setTraceSigInt, 'function');
    assert.strictEqual(typeof sysDefault.setTraceSigInt, 'function');

    assert.strictEqual(utilMod.setTraceSigInt(true), undefined);
    assert.strictEqual(utilDefault.setTraceSigInt(false), undefined);
  },
};

export const utilDiffTest = {
  test() {
    assert.strictEqual(typeof utilMod.diff, 'function');
    assert.strictEqual(typeof utilDefault.diff, 'function');
    assert.strictEqual(typeof sysMod.diff, 'function');
    assert.strictEqual(typeof sysDefault.diff, 'function');

    assert.throws(() => utilMod.diff(1, 2), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    assert.throws(() => utilDefault.diff('a', 'b', {}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    assert.throws(() => sysMod.diff(1, 2), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const utilConvertProcessSignalToExitCodeTest = {
  test() {
    assert.strictEqual(
      typeof utilMod.convertProcessSignalToExitCode,
      'function'
    );
    assert.strictEqual(
      typeof utilDefault.convertProcessSignalToExitCode,
      'function'
    );
    assert.strictEqual(
      typeof sysMod.convertProcessSignalToExitCode,
      'function'
    );
    assert.strictEqual(
      typeof sysDefault.convertProcessSignalToExitCode,
      'function'
    );

    assert.throws(() => utilMod.convertProcessSignalToExitCode('SIGINT'), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
    assert.throws(() => utilDefault.convertProcessSignalToExitCode(2), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};
