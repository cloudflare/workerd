// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class ObservedR2Binding extends WorkerEntrypoint {
  fetch() {
    throw new Error('R2 platform binding tests must not use HTTP');
  }

  async head(...args) {
    if (args[0] === 'body-as-head') {
      return this.env.REAL_BUCKET.get('rpc-json');
    }
    if (args[0] === 'list-object-as-head') {
      const listed = await this.env.REAL_BUCKET.list({
        prefix: 'httpMeta',
        include: [],
      });
      return listed.objects[0];
    }
    return this.env.REAL_BUCKET.head(...args);
  }

  async get(...args) {
    if (args[0] === 'list-object-as-get') {
      const listed = await this.env.REAL_BUCKET.list({
        prefix: 'httpMeta',
        include: [],
      });
      return listed.objects[0];
    }
    return this.env.REAL_BUCKET.get(...args);
  }

  put(...args) {
    return this.env.REAL_BUCKET.put(...args);
  }

  list(...args) {
    return this.env.REAL_BUCKET.list(...args);
  }

  async checksums() {
    return (await this.env.REAL_BUCKET.head('multipleChecksums')).checksums;
  }

  getNative(key) {
    return this.env.REAL_BUCKET.get(key);
  }

  async createMultipartUpload(...args) {
    const upload = await this.env.REAL_BUCKET.createMultipartUpload(...args);
    return { key: upload.key, uploadId: upload.uploadId };
  }

  uploadPart(key, uploadId, partNumber, value, options) {
    return this.env.REAL_BUCKET.resumeMultipartUpload(key, uploadId).uploadPart(
      partNumber,
      value,
      options
    );
  }

  abortMultipartUpload(key, uploadId) {
    return this.env.REAL_BUCKET.resumeMultipartUpload(key, uploadId).abort();
  }

  completeMultipartUpload(key, uploadId, uploadedParts) {
    return this.env.REAL_BUCKET.resumeMultipartUpload(key, uploadId).complete(
      uploadedParts
    );
  }
}

export const nativeR2ResultTests = {
  async test(_controller, env) {
    const checksums = await env.PLATFORM.checksums();
    assert.throws(() => structuredClone(checksums), { name: 'DataCloneError' });
    const checksumJson = checksums.toJSON();
    assert.strictEqual(checksumJson.md5, '9a0364b9e99bb480dd25e1f0284c8555');
    assert.strictEqual(
      checksumJson.sha1,
      '2a0364b9e99bb480dd25e1f0284c855511223344'
    );
    assert.strictEqual(
      checksumJson.sha256,
      '3a0364b9e99bb480dd25e1f0284c8555112233445566778899aabbccddeeff00'
    );

    const head = await env.BUCKET.head('multipleChecksums');
    assert.throws(() => structuredClone(head), { name: 'DataCloneError' });
    assert.strictEqual(head.key, 'basicKey');
    assert.strictEqual(typeof head.writeHttpMetadata, 'function');
    const headChecksumJson = head.checksums.toJSON();
    assert.strictEqual(
      headChecksumJson.md5,
      '9a0364b9e99bb480dd25e1f0284c8555'
    );
    assert.strictEqual(
      headChecksumJson.sha1,
      '2a0364b9e99bb480dd25e1f0284c855511223344'
    );
    assert.strictEqual(
      headChecksumJson.sha256,
      '3a0364b9e99bb480dd25e1f0284c8555112233445566778899aabbccddeeff00'
    );
    await assert.rejects(() => env.BUCKET.head('body-as-head'), {
      message: /^internal error; reference = \S+$/,
    });
    await assert.rejects(() => env.BUCKET.head('list-object-as-head'), {
      message: /^internal error; reference = \S+$/,
    });

    const directGet = await env.PLATFORM.getNative('rpc-json');
    assert.throws(() => structuredClone(directGet), { name: 'DataCloneError' });
    assert.deepStrictEqual(await directGet.json(), { ok: true });

    const object = await env.BUCKET.get('rpc-json');
    assert.strictEqual(typeof object.text, 'function');
    assert.deepStrictEqual(await object.json(), { ok: true });

    const metadataOnly = await env.BUCKET.get('rpc-conditional-metadata');
    assert.strictEqual(metadataOnly.key, 'basicKey');
    assert.strictEqual(metadataOnly.body, undefined);
    await assert.rejects(() => env.BUCKET.get('list-object-as-get'), {
      message: /^internal error; reference = \S+$/,
    });

    const put = await env.BUCKET.put('platform-put', 'content');
    assert.strictEqual(put.key, 'basicKey');
    assert.strictEqual(typeof put.writeHttpMetadata, 'function');

    const listed = await env.BUCKET.list({
      prefix: 'rpc-metadata',
      include: ['httpMetadata', 'customMetadata'],
    });
    assert.strictEqual(listed.objects.length, 1);
    assert.strictEqual(listed.objects[0].key, 'basicKey');
    assert.strictEqual(typeof listed.objects[0].writeHttpMetadata, 'function');

    const listedWithoutMetadata = await env.BUCKET.list({
      prefix: 'httpMeta',
      include: [],
    });
    assert.strictEqual(
      listedWithoutMetadata.objects[0].httpMetadata,
      undefined
    );
    assert.strictEqual(
      listedWithoutMetadata.objects[0].customMetadata,
      undefined
    );

    const upload = await env.BUCKET.createMultipartUpload(
      'rpc-multipart-platform'
    );
    assert.strictEqual(upload.key, 'rpc-multipart-platform');
    assert.strictEqual(upload.uploadId, 'rpc-multipart-platform-id');
    const part = await upload.uploadPart(1, 'content');
    assert.strictEqual(
      part.etag,
      'rpc-multipart-platform/rpc-multipart-platform-id/1'
    );
    const completed = await upload.complete([part]);
    assert.strictEqual(completed.key, 'rpc-multipart-platform');
    assert.strictEqual(completed.version, 'rpc-multipart-platform-id');

    const aborted = await env.BUCKET.createMultipartUpload(
      'rpc-multipart-abort-platform'
    );
    await aborted.abort();
  },
};
