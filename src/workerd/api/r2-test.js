// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

const bufferKey = new Uint8Array([
  185, 255, 145, 154, 120, 76, 122, 72, 191, 42, 8, 64, 86, 189, 185, 75, 105,
  37, 155, 123, 165, 158, 4, 42, 222, 13, 135, 52, 87, 154, 181, 227,
]);
const hexKey =
  'b9ff919a784c7a48bf2a084056bdb94b69259b7ba59e042ade0d8734579ab5e3';
const keyMd5 = 'WGR5pEm07DroP3hYRAh8Yw==';

const objResponse = {
  name: 'objectKey',
  version: 'objectVersion',
  size: '123',
  etag: 'objectEtag',
  uploaded: '1724767257918',
  storageClass: 'Standard',
};

export default {
  // Handler for HTTP request binding makes to R2
  async fetch(request, env, ctx) {
    // We only expect PUT/Get
    assert(['GET', 'PUT'].includes(request.method));

    switch (request.method) {
      case 'PUT': {
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
        assert(
          [
            'put',
            'createMultipartUpload',
            'uploadPart',
            'completeMultipartUpload',
          ].includes(jsonRequest.method)
        );

        switch (jsonRequest.object) {
          case 'onlyIfStrongEtag': {
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
            break;
          }
          case 'onlyIfWildcard': {
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
            break;
          }
          case 'ssec': {
            assert.deepStrictEqual(jsonRequest.ssec, {
              key: hexKey,
            });
            return Response.json({
              ...objResponse,
              ssec: {
                algorithm: 'aes256',
                keyMd5,
              },
            });
          }
          case 'ssec-mu': {
            if (jsonRequest.method === 'createMultipartUpload') {
              assert.deepStrictEqual(jsonRequest.ssec, {
                key: hexKey,
              });
              return Response.json({
                uploadId: 'definitelyARealId',
              });
            }
            if (jsonRequest.method === 'uploadPart') {
              assert.deepStrictEqual(jsonRequest.ssec, {
                key: hexKey,
              });
              return Response.json({
                etag: 'definitelyAValidEtag',
                ssec: {
                  algorithm: 'aes256',
                  keyMd5,
                },
              });
            }
            if (jsonRequest.method === 'completeMultipartUpload') {
              return Response.json({
                ...objResponse,
                ssec: {
                  algorithm: 'aes256',
                  keyMd5,
                },
              });
            }
          }
        }
        return Response.json(objResponse);
      }
      case 'GET': {
        const rawHeader = request.headers.get('cf-r2-request');
        const jsonRequest = JSON.parse(rawHeader);
        assert((jsonRequest.version = 1));
        assert(['get', 'head'].includes(jsonRequest.method));
        if (jsonRequest.object === 'ssec') {
          const encoder = new TextEncoder();
          const metadata = encoder.encode(
            JSON.stringify({
              ...objResponse,
              ssec: {
                algorithm: 'aes256',
                keyMd5,
              },
            })
          );
          const body =
            jsonRequest.method === 'get'
              ? new ReadableStream({
                  start(controller) {
                    controller.enqueue(metadata);
                    controller.enqueue(encoder.encode('Bonk'));
                    controller.close();
                  },
                })
              : metadata;
          return new Response(body, {
            headers: {
              'cf-r2-metadata-size': metadata.length.toString(),
              'content-length': metadata.length.toString(),
            },
          });
        }
      }
    }
    throw new Error('unexpected');
  },

  async test(ctrl, env, ctx) {
    {
      // Conditionals
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
    }

    {
      // SSEC
      for (const ssecKey of [bufferKey, hexKey]) {
        {
          const { ssecKeyMd5 } = await env.BUCKET.put('ssec', 'content', {
            ssecKey,
          });
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
        {
          const { ssecKeyMd5 } = await env.BUCKET.get('ssec', {
            ssecKey,
          });
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
        {
          const { ssecKeyMd5 } = await env.BUCKET.head('ssec', {
            ssecKey,
          });
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
        {
          const multi = await env.BUCKET.createMultipartUpload('ssec-mu', {
            ssecKey,
          });
          assert.equal(multi.uploadId, 'definitelyARealId');
        }
        {
          const multi = await env.BUCKET.createMultipartUpload('ssec-mu', {
            ssecKey,
          });
          const part = await multi.uploadPart(1, 'hey', {
            ssecKey,
          });
          assert.equal(part.etag, 'definitelyAValidEtag');
        }
        {
          const multi = await env.BUCKET.createMultipartUpload('ssec-mu', {
            ssecKey,
          });
          const { ssecKeyMd5 } = await multi.complete([
            {
              partNumber: 1,
              etag: 'definitelyAValidEtag',
            },
          ]);
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
      }
    }
  },
};
