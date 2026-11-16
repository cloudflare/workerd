// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { ObservedR2Binding as PassthroughR2Binding } from './r2-observed-binding.js';

// Exposes native R2 resources through an ordinary service binding so this suite exercises their
// JSRPC serializers without involving the R2 binding's named-method client.
export class ResourceSerializationEntrypoint extends PassthroughR2Binding {
  async checksums() {
    return (await this.env.REAL_BUCKET.head('multipleChecksums')).checksums;
  }

  headNative(requestKey) {
    return this.env.REAL_BUCKET.head(requestKey);
  }

  getNative(requestKey) {
    return this.env.REAL_BUCKET.get(requestKey);
  }

  echoNative(object) {
    return object;
  }
}

export const resourceSerializationTests = {
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

    const head = await env.PLATFORM.headNative('multipleChecksums');
    assert.throws(() => structuredClone(head), { name: 'DataCloneError' });
    assert.strictEqual(head.key, 'basicKey');
    assert.strictEqual(typeof head.writeHttpMetadata, 'function');
    assert.deepStrictEqual(head.checksums.toJSON(), checksumJson);

    const directGet = await env.PLATFORM.getNative('rpc-json');
    assert.throws(() => structuredClone(directGet), { name: 'DataCloneError' });
    assert.deepStrictEqual(await directGet.json(), { ok: true });
    assert.strictEqual(directGet.bodyUsed, true);

    // Each consumption method must work without first accessing the body getter.
    for (const method of ['arrayBuffer', 'bytes', 'text', 'blob']) {
      const result = await env.PLATFORM.getNative('rpc-lazy-body');
      const consumed = await result[method]();
      assert.strictEqual(await new Response(consumed).text(), 'content');
      assert.strictEqual(result.bodyUsed, true);
    }

    // A deserialized native result must be serializable again before its body is accessed.
    const echoed = await env.PLATFORM.echoNative(
      await env.PLATFORM.getNative('rpc-json')
    );
    assert.deepStrictEqual(await echoed.json(), { ok: true });

    // The resource serializer preserves a body stream's known length across a service RPC hop.
    const source = await env.PLATFORM.getNative('rpc-lazy-body');
    const copied = await env.BUCKET.put('basicKey', source.body);
    assert.strictEqual(copied.size, 7);
    assert.strictEqual(source.bodyUsed, true);
  },
};
