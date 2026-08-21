// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

// Stands in for the gateway's R2BindingEntrypoint. It returns the values the real gateway returns
// after its own parsing -- plain data in the binding's vocabulary, with a real Date and real
// ArrayBuffers -- rather than the HTTP wire shape. The C++ side rebuilds R2Object from this.
//
// Written out literally rather than generated, because the RPC contract between the two repos is
// the thing under test; a shared builder would let both sides drift together.

const uploadedMs = 1700000000000;

function bytes(...values) {
  return new Uint8Array(values).buffer;
}

function baseObject(overrides = {}) {
  return {
    key: 'some/key',
    version: '00000000-0000-0000-0000-000000000001',
    size: 1024,
    etag: 'abc123',
    uploaded: new Date(uploadedMs),
    storageClass: 'Standard',
    checksums: {},
    ...overrides,
  };
}

export class R2BindingEntrypoint extends WorkerEntrypoint {
  async head(key) {
    switch (key) {
      case 'missing':
        // Object not found is null, not an error.
        return null;
      case 'checksums':
        return baseObject({
          checksums: { md5: bytes(0xff, 0x00), sha1: bytes(0x01, 0x02) },
        });
      case 'metadata':
        return baseObject({
          httpMetadata: {
            contentType: 'text/plain',
            cacheExpiry: new Date(uploadedMs + 1000),
          },
          customMetadata: { colour: 'blue' },
        });
      case 'ranged':
        return baseObject({ range: { offset: 10, length: 20 } });
      case 'ssec':
        return baseObject({ ssecKeyMd5: 'deadbeef' });
      case 'boom':
        // The gateway formats errors as `<action>: <message> (<v4code>)` and throws a plain Error,
        // reproducing what R2Result::throwIfError produces on the HTTP path.
        throw new Error('head: no such bucket (10006)');
      default:
        return baseObject();
    }
  }

  async delete(keys) {
    if (keys === 'boom') {
      throw new Error('delete: bad keys (10021)');
    }
    // Record what arrived so the test can assert the union survived marshalling.
    globalThis.lastDeleteArg = keys;
  }
}

export const test = {
  async test(ctrl, env, ctx) {
    // Dispatch reaches the named entrypoint at all. If the binding targeted the default export, or
    // either gate were off, this would throw "does not implement the method".
    {
      const obj = await env.BUCKET.head('plain');
      assert.strictEqual(obj.key, 'some/key');
      assert.strictEqual(obj.version, '00000000-0000-0000-0000-000000000001');
      assert.strictEqual(obj.size, 1024);
      assert.strictEqual(obj.etag, 'abc123');
      assert.strictEqual(obj.storageClass, 'Standard');
      assert.strictEqual(obj.uploaded.getTime(), uploadedMs);
    }

    // The result is a real R2Object, not the plain data the gateway sent. This is what a raw
    // passthrough would lose.
    {
      const obj = await env.BUCKET.head('plain');
      assert.strictEqual(typeof obj.writeHttpMetadata, 'function');
      assert.strictEqual(typeof obj.checksums.toJSON, 'function');
      assert.strictEqual(obj.httpEtag, '"abc123"');
    }

    // Absent metadata becomes empty rather than undefined, matching the HTTP path. GetResult later
    // hard-asserts both are present, so this is load-bearing beyond head().
    {
      const obj = await env.BUCKET.head('plain');
      assert.deepStrictEqual(obj.httpMetadata, {});
      assert.deepStrictEqual(obj.customMetadata, {});
    }

    {
      const obj = await env.BUCKET.head('missing');
      assert.strictEqual(obj, null);
    }

    {
      const obj = await env.BUCKET.head('checksums');
      assert.deepStrictEqual(
        new Uint8Array(obj.checksums.md5),
        new Uint8Array([0xff, 0x00])
      );
      assert.deepStrictEqual(
        new Uint8Array(obj.checksums.sha1),
        new Uint8Array([0x01, 0x02])
      );
      assert.strictEqual(obj.checksums.sha256, undefined);
      assert.deepStrictEqual(obj.checksums.toJSON(), {
        md5: 'ff00',
        sha1: '0102',
      });
    }

    {
      const obj = await env.BUCKET.head('metadata');
      assert.strictEqual(obj.httpMetadata.contentType, 'text/plain');
      assert.strictEqual(
        obj.httpMetadata.cacheExpiry.getTime(),
        uploadedMs + 1000
      );
      assert.deepStrictEqual(obj.customMetadata, { colour: 'blue' });

      const headers = new Headers();
      obj.writeHttpMetadata(headers);
      assert.strictEqual(headers.get('content-type'), 'text/plain');
    }

    {
      const obj = await env.BUCKET.head('ranged');
      assert.strictEqual(obj.range.offset, 10);
      assert.strictEqual(obj.range.length, 20);
    }

    {
      const obj = await env.BUCKET.head('ssec');
      assert.strictEqual(obj.ssecKeyMd5, 'deadbeef');
    }

    // Errors cross RPC as a plain Error with the message already formatted. The v4 code is part of
    // the text, not a property -- R2Result::throwIfError's structured R2Error throw is compiled out
    // under `#if 0`, so pinning `.code === undefined` guards against silently changing that.
    {
      await assert.rejects(env.BUCKET.head('boom'), (err) => {
        assert.strictEqual(err.message, 'head: no such bucket (10006)');
        assert.strictEqual(err.code, undefined);
        return true;
      });
    }

    // delete resolves to undefined and forwards the string/array union unchanged.
    {
      assert.strictEqual(await env.BUCKET.delete('one/key'), undefined);
      assert.strictEqual(globalThis.lastDeleteArg, 'one/key');

      await env.BUCKET.delete(['a', 'b', 'c']);
      assert.deepStrictEqual(globalThis.lastDeleteArg, ['a', 'b', 'c']);
    }

    {
      await assert.rejects(env.BUCKET.delete('boom'), (err) => {
        assert.strictEqual(err.message, 'delete: bad keys (10021)');
        return true;
      });
    }

    // Argument coercion still happens client-side, because the C++ method keeps a typed kj::String
    // parameter. A raw FunctionCallbackInfo passthrough would have sent the number through.
    {
      const obj = await env.BUCKET.head(12345);
      assert.strictEqual(obj.key, 'some/key');
    }
  },
};
