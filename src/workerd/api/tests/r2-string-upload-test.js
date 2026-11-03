// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { stringUploadCases } from './r2-test.js';

function stringUploadValue({ size, suffix }) {
  return (
    'a'.repeat(size - new TextEncoder().encode(suffix).byteLength) + suffix
  );
}

export const r2StringPutTests = {
  async test(_ctrl, env) {
    for (const [key, fixture] of Object.entries(stringUploadCases)) {
      const result = await env.BUCKET.put(key, stringUploadValue(fixture));
      assert.strictEqual(result.key, key);
      assert.strictEqual(result.size, fixture.size);
    }
  },
};

export const r2StringUploadPartTests = {
  async test(_ctrl, env) {
    for (const [key, fixture] of Object.entries(stringUploadCases)) {
      const upload = env.BUCKET.resumeMultipartUpload(key, 'string-upload-id');
      const result = await upload.uploadPart(1, stringUploadValue(fixture));
      assert.deepStrictEqual(result, {
        partNumber: 1,
        etag: `string-upload-${fixture.size}`,
      });
    }
  },
};
