// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import sea, {
  isSea,
  getAssetKeys,
  getAsset,
  getAssetAsBlob,
  getRawAsset,
} from 'node:sea';

export const seaIsSeaTest = {
  test() {
    assert.strictEqual(isSea(), false);
    assert.strictEqual(sea.isSea(), false);
  },
};

export const seaGetAssetKeysTest = {
  test() {
    assert.deepStrictEqual(getAssetKeys(), []);
    assert.deepStrictEqual(sea.getAssetKeys(), []);
  },
};

export const seaAssetLookupThrowsTest = {
  test() {
    assert.throws(() => getAsset('foo'), /not found/);
    assert.throws(() => getAssetAsBlob('foo'), /not found/);
    assert.throws(() => getRawAsset('foo'), /not found/);
    assert.throws(() => sea.getAsset('foo'), /not found/);
    assert.throws(() => sea.getAssetAsBlob('foo'), /not found/);
    assert.throws(() => sea.getRawAsset('foo'), /not found/);
  },
};
