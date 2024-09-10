// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export default {
  // Handler for HTTP request binding makes to R2
  async fetch(request, env, ctx) {
    // We only expect PUT/Get
    assert(['GET', 'PUT'].includes(request.method));

    // Each request should have a metadata size header indicating how much
    // we should read to understand what type of request this is
    const metadataSizeString = request.headers.get('cf-r2-metadata-size');
    assert.notStrictEqual(metadataSizeString, null);

    const metadataSize = parseInt(metadataSizeString);
    assert(!Number.isNaN(metadataSize));

    const reader = request.body.getReader({ mode: 'byob' });
    const jsonArray = new Uint8Array(metadataSize);
    const { value } = await reader.readAtLeast(metadataSize, jsonArray);
    reader.releaseLock();

    const jsonRequest = JSON.parse(new TextDecoder().decode(value));

    // Currently not using the body in these test so I'm going to just discard
    for await (const _ of request.body) {
    }

    // Assert it's the correct version
    assert((jsonRequest.version = 1));

    if (request.method === 'PUT') {
      assert(jsonRequest.method === 'put');

      if (jsonRequest.object === 'onlyIfStrongEtag') {
        assert.deepStrictEqual(jsonRequest.onlyIf, {
          etagMatches: [
            {
              value: 'strongEtag',
              type: 'strong',
            },
          ],
          etagDoesNotMatch: [
            {
              value: 'strongEtag',
              type: 'strong',
            },
          ],
        });
      }

      if (jsonRequest.object === 'onlyIfWildcard') {
        assert.deepStrictEqual(jsonRequest.onlyIf, {
          etagMatches: [
            {
              type: 'wildcard',
            },
          ],
          etagDoesNotMatch: [
            {
              type: 'wildcard',
            },
          ],
        });
      }

      return Response.json({
        name: 'objectKey',
        version: 'objectVersion',
        size: '123',
        etag: 'objectEtag',
        uploaded: '1724767257918',
        storageClass: 'Standard',
      });
    }

    throw new Error('unexpected');
  },

  async test(ctrl, env, ctx) {
    await env.BUCKET.put('basic', 'content');
    await env.BUCKET.put('onlyIfStrongEtag', 'content', {
      onlyIf: {
        etagMatches: 'strongEtag',
        etagDoesNotMatch: 'strongEtag',
      },
    });
    await env.BUCKET.put('onlyIfWildcard', 'content', {
      onlyIf: {
        etagMatches: '*',
        etagDoesNotMatch: '*',
      },
    });
  },
};
