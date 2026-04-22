// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

import * as fsMod from 'node:fs';
import fsDefault from 'node:fs';
import * as fsPromises from 'node:fs/promises';
import fsPromisesDefault from 'node:fs/promises';

export const fsUtf8StreamTest = {
  test() {
    assert.strictEqual(typeof fsMod.Utf8Stream, 'function');
    assert.strictEqual(typeof fsDefault.Utf8Stream, 'function');
    assert.throws(() => new fsMod.Utf8Stream(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const fsMkdtempDisposableSyncTest = {
  test() {
    assert.strictEqual(typeof fsMod.mkdtempDisposableSync, 'function');
    assert.strictEqual(typeof fsDefault.mkdtempDisposableSync, 'function');

    const disposable = fsMod.mkdtempDisposableSync('/tmp/diff-exports-');
    try {
      assert.strictEqual(typeof disposable.path, 'string');
      assert.ok(disposable.path.startsWith('/tmp/diff-exports-'));
      assert.strictEqual(typeof disposable.remove, 'function');
      assert.strictEqual(typeof disposable[Symbol.dispose], 'function');
    } finally {
      disposable.remove();
      // Idempotent.
      disposable.remove();
    }
  },
};

export const fsPromisesMkdtempDisposableTest = {
  test: async () => {
    assert.strictEqual(typeof fsPromises.mkdtempDisposable, 'function');
    assert.strictEqual(typeof fsPromisesDefault.mkdtempDisposable, 'function');

    const disposable = await fsPromises.mkdtempDisposable(
      '/tmp/diff-exports-promises-'
    );
    try {
      assert.strictEqual(typeof disposable.path, 'string');
      assert.ok(disposable.path.startsWith('/tmp/diff-exports-promises-'));
      assert.strictEqual(typeof disposable.remove, 'function');
      assert.strictEqual(typeof disposable[Symbol.asyncDispose], 'function');
    } finally {
      await disposable.remove();
      await disposable.remove();
    }
  },
};
