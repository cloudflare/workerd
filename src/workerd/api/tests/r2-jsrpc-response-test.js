// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import mock from './r2-test.js';
import { ObservedR2Binding as PassthroughR2Binding } from './r2-observed-binding.js';

// Returns native R2 resources that violate a named method's result contract.
export class InvalidR2ResponseEntrypoint extends PassthroughR2Binding {
  async head(requestKey) {
    if (requestKey === 'body-as-head') {
      return this.env.REAL_BUCKET.get('rpc-json');
    }
    if (requestKey === 'list-object-as-head') {
      const listed = await this.env.REAL_BUCKET.list({
        prefix: 'httpMeta',
        include: [],
      });
      return listed.objects[0];
    }
    return super.head(requestKey);
  }

  async get(requestKey, options) {
    if (requestKey === 'list-object-as-get') {
      const listed = await this.env.REAL_BUCKET.list({
        prefix: 'httpMeta',
        include: [],
      });
      return listed.objects[0];
    }
    return super.get(requestKey, options);
  }

  async createMultipartUpload(requestKey, options) {
    if (requestKey === 'upload-id-as-string') {
      return 'upload-id';
    }
    if (requestKey === 'upload-id-as-number') {
      return { uploadId: 123 };
    }
    if (requestKey === 'upload-id-missing') {
      return {};
    }
    if (requestKey === 'upload-id-with-key') {
      return { key: requestKey, uploadId: 'upload-id' };
    }
    return super.createMultipartUpload(requestKey, options);
  }
}

export default {
  fetch: mock.fetch,

  async test(_controller, env) {
    const internalError = { message: /^internal error; reference = \S+$/ };
    await assert.rejects(() => env.BUCKET.head('body-as-head'), internalError);
    await assert.rejects(
      () => env.BUCKET.head('list-object-as-head'),
      internalError
    );
    await assert.rejects(
      () => env.BUCKET.get('list-object-as-get'),
      internalError
    );

    for (const key of [
      'upload-id-as-string',
      'upload-id-as-number',
      'upload-id-missing',
    ]) {
      await assert.rejects(
        () => env.BUCKET.createMultipartUpload(key),
        internalError
      );
    }

    // Extra properties do not prevent construction of a valid local multipart resource.
    const upload = await env.BUCKET.createMultipartUpload('upload-id-with-key');
    assert.strictEqual(upload.key, 'upload-id-with-key');
    assert.strictEqual(upload.uploadId, 'upload-id');
  },
};
