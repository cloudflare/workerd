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
const conditionalDate = '946684800000';

const objResponse = {
  name: 'basicKey',
  version: 'objectVersion',
  size: '123',
  etag: 'objectEtag',
  uploaded: '1724767257918',
  storageClass: 'Standard',
};
const HeadObject = {
  ssecKeyMd5: undefined,
  storageClass: 'Standard',
  range: undefined,
  customMetadata: {},
  httpMetadata: {},
  uploaded: new Date(Number(objResponse.uploaded)),
  checksums: {
    sha512: undefined,
    sha384: undefined,
    sha256: undefined,
    sha1: undefined,
    md5: undefined,
  },
  httpEtag: '"objectEtag"',
  etag: 'objectEtag',
  size: 123,
  version: 'objectVersion',
  key: 'basicKey',
};

function buildGetResponse(meta, body) {
  const encoder = new TextEncoder();
  const metadata = encoder.encode(JSON.stringify(meta));
  const responseBody = body
    ? new ReadableStream({
        start(controller) {
          controller.enqueue(metadata);
          controller.enqueue(encoder.encode(body));
          controller.close();
        },
      })
    : metadata;
  return new Response(responseBody, {
    headers: {
      'cf-r2-metadata-size': metadata.length.toString(),
      'content-length': metadata.length.toString(),
    },
  });
}

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

        if (jsonRequest.method === 'delete') {
          if (jsonRequest.objects) {
            assert.deepEqual(jsonRequest.objects, ['basicKey', 'basicKey2']);
          } else {
            assert.deepEqual(jsonRequest.object, 'basicKey');
          }
          return new Response();
        }

        switch (jsonRequest.object) {
          case 'basicKey': {
            assert.strictEqual(jsonRequest.method, 'put');
            break;
          }
          case 'multiKey': {
            if (jsonRequest.method === 'createMultipartUpload') {
              return Response.json({
                uploadId: 'multipartId',
              });
            }

            if (jsonRequest.method === 'uploadPart') {
              return Response.json({
                etag: 'partEtag',
              });
            }
            if (jsonRequest.method === 'completeMultipartUpload') {
              return Response.json({
                ...objResponse,
                name: 'multiKey',
              });
            }
            break;
          }
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
              uploadedBefore: conditionalDate,
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
              uploadedAfter: conditionalDate,
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
          case 'ssecMu': {
            if (jsonRequest.method === 'createMultipartUpload') {
              assert.deepStrictEqual(jsonRequest.ssec, {
                key: hexKey,
              });
              return Response.json({
                uploadId: 'multipartId',
              });
            }
            if (jsonRequest.method === 'uploadPart') {
              assert.deepStrictEqual(jsonRequest.ssec, {
                key: hexKey,
              });
              return Response.json({
                etag: 'partEtag',
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
        if (jsonRequest.method === 'list') {
          assert.deepEqual(jsonRequest, {
            version: 1,
            method: 'list',
            limit: 1,
            prefix: 'basic',
            cursor: 'ai',
            delimiter: '/',
            include: [0, 1],
            newRuntime: true,
          });
          return buildGetResponse({
            objects: [objResponse],
            truncated: false,
            cursor: 'ai',
            deliminatedPrefixes: [],
          });
        }
        assert(['get', 'head'].includes(jsonRequest.method));
        switch (jsonRequest.object) {
          case 'basicKey': {
            return buildGetResponse(objResponse, 'content');
          }
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
              uploadedBefore: conditionalDate,
            });
            return buildGetResponse(objResponse, 'content');
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
              uploadedAfter: conditionalDate,
            });
            return buildGetResponse(objResponse, 'content');
          }
          case 'ssec': {
            return buildGetResponse(
              {
                ...objResponse,
                ssec: {
                  algorithm: 'aes256',
                  keyMd5,
                },
              },
              'content'
            );
          }
        }
      }
    }
    throw new Error('unexpected');
  },
  async test(ctrl, env, ctx) {
    // Basic Operations
    {
      // Destructuring syntax looks ugly, but gets around needing to construct HeadResponse objects(somehow?)
      // PutObject
      {
        const { ...obj } = await env.BUCKET.put('basicKey', 'content');
        obj.checksums = { ...obj.checksums };
        assert.deepEqual(obj, HeadObject);
      }
      // GetObject
      {
        const objWithBody = await env.BUCKET.get('basicKey');
        const { ...obj } = objWithBody;
        obj.checksums = { ...obj.checksums };
        assert.deepEqual(obj, HeadObject);
        assert.deepEqual(await objWithBody.text(), 'content');
      }
      // HeadObject
      {
        const { ...obj } = await env.BUCKET.head('basicKey');
        obj.checksums = { ...obj.checksums };
        assert.deepEqual(obj, HeadObject);
      }
      // MultipartUploads
      {
        // CreateMultipartUpload
        const multi = await env.BUCKET.createMultipartUpload('multiKey');
        assert.equal(multi.uploadId, 'multipartId');
        assert.equal(multi.key, 'multiKey');
        // UploadPart
        const part = await multi.uploadPart(1, 'content');
        assert.equal(part.etag, 'partEtag');
        // CompleteMultipartUpload
        const { ...obj } = await multi.complete([
          {
            partNumber: 1,
            etag: 'partEtag',
          },
        ]);
        obj.checksums = { ...obj.checksums };
        assert.deepEqual(obj, { ...HeadObject, key: 'multiKey' });
      }
      // ListObjects
      {
        const list = await env.BUCKET.list({
          limit: 1,
          prefix: 'basic',
          cursor: 'ai',
          delimiter: '/',
          include: ['httpMetadata', 'customMetadata'],
        });
        list.objects[0] = { ...list.objects[0] };
        list.objects[0].checksums = { ...list.objects[0].checksums };
        assert.deepEqual(list, {
          objects: [HeadObject],
          truncated: false,
          cursor: 'ai',
          delimitedPrefixes: [],
        });
      }
      // DeleteObject
      {
        await env.BUCKET.delete('basicKey');
        await env.BUCKET.delete(['basicKey', 'basicKey2']);
      }
    }
    // Conditionals
    {
      await env.BUCKET.put('onlyIfStrongEtag', 'content', {
        onlyIf: {
          etagMatches: 'strongEtag',
          etagDoesNotMatch: 'strongEtag',
          uploadedBefore: new Date('0'),
        },
      });
      await env.BUCKET.put('onlyIfWildcard', 'content', {
        onlyIf: {
          etagMatches: '*',
          etagDoesNotMatch: '*',
          uploadedAfter: new Date('0'),
        },
      });
      await env.BUCKET.get('onlyIfStrongEtag', {
        onlyIf: {
          etagMatches: 'strongEtag',
          etagDoesNotMatch: 'strongEtag',
          uploadedBefore: new Date('0'),
        },
      });
      await env.BUCKET.get('onlyIfWildcard', {
        onlyIf: {
          etagMatches: '*',
          etagDoesNotMatch: '*',
          uploadedAfter: new Date('0'),
        },
      });
    }
    // SSEC
    {
      for (const ssecKey of [bufferKey, hexKey]) {
        // PutObject
        {
          const { ssecKeyMd5 } = await env.BUCKET.put('ssec', 'content', {
            ssecKey,
          });
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
        // GetObject
        {
          const { ssecKeyMd5 } = await env.BUCKET.get('ssec', {
            ssecKey,
          });
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
        // HeadObject
        {
          const { ssecKeyMd5 } = await env.BUCKET.head('ssec', {
            ssecKey,
          });
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
        // MultipartUpload
        {
          // CreateMultipartUpload
          const multi = await env.BUCKET.createMultipartUpload('ssecMu', {
            ssecKey,
          });
          assert.equal(multi.uploadId, 'multipartId');
          // UploadPart
          const part = await multi.uploadPart(1, 'hey', {
            ssecKey,
          });
          assert.equal(part.etag, 'partEtag');
          // CompleteMultipartUpload
          const { ssecKeyMd5 } = await multi.complete([
            {
              partNumber: 1,
              etag: 'partEtag',
            },
          ]);
          assert.strictEqual(ssecKeyMd5, keyMd5);
        }
      }
    }
  },
};
