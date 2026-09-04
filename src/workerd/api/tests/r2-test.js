// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { RpcTarget, WorkerEntrypoint } from 'cloudflare:workers';

const key = 'basicKey';
const body = 'content';
const rpcStreamBody = 'café';
const rpcInlineBodyLimit = 16 << 20;
const largeRpcBodySize = rpcInlineBodyLimit + 2;
const httpMetaObj = {
  contentType: 'text/plain',
  contentLanguage: 'en-US',
  contentDisposition: 'attachment; filename = "basicKey.txt"',
  contentEncoding: 'utf-8',
  cacheControl: 'no-store',
  cacheExpiry: new Date(1e3),
};
const httpFields = {
  ...httpMetaObj,
  cacheExpiry: '1000',
};
const httpMetaHeaders = new Headers({
  'content-type': httpMetaObj.contentType,
  'content-language': httpMetaObj.contentLanguage,
  'content-disposition': httpMetaObj.contentDisposition,
  'content-encoding': httpMetaObj.contentEncoding,
  'cache-control': httpMetaObj.cacheControl,
  expires: httpMetaObj.cacheExpiry.toUTCString(),
});
const customMetadata = {
  foo: 'bar',
  baz: 'qux',
};
const customFields = Object.entries(customMetadata).map(([k, v]) => ({ k, v }));
const bufferKey = new Uint8Array([
  185, 255, 145, 154, 120, 76, 122, 72, 191, 42, 8, 64, 86, 189, 185, 75, 105,
  37, 155, 123, 165, 158, 4, 42, 222, 13, 135, 52, 87, 154, 181, 227,
]);
const hexKey =
  'b9ff919a784c7a48bf2a084056bdb94b69259b7ba59e042ade0d8734579ab5e3';
const keyMd5 = 'WGR5pEm07DroP3hYRAh8Yw==';
const conditionalDate = '946684800000';

// Test checksums - known values for testing
const md5Buffer = new Uint8Array([
  0x9a, 0x03, 0x64, 0xb9, 0xe9, 0x9b, 0xb4, 0x80, 0xdd, 0x25, 0xe1, 0xf0, 0x28,
  0x4c, 0x85, 0x55,
]);
// Test SHA1 checksum
const sha1Buffer = new Uint8Array([
  0x2a, 0x03, 0x64, 0xb9, 0xe9, 0x9b, 0xb4, 0x80, 0xdd, 0x25, 0xe1, 0xf0, 0x28,
  0x4c, 0x85, 0x55, 0x11, 0x22, 0x33, 0x44,
]);
// Test SHA256 checksum
const sha256Buffer = new Uint8Array([
  0x3a, 0x03, 0x64, 0xb9, 0xe9, 0x9b, 0xb4, 0x80, 0xdd, 0x25, 0xe1, 0xf0, 0x28,
  0x4c, 0x85, 0x55, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa,
  0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
]);
const objResponse = {
  name: key,
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
  key,
};

function buildRpcHead(requestKey, multipartOptions) {
  const result = {
    key,
    version: objResponse.version,
    size: Number(objResponse.size),
    etag: objResponse.etag,
    uploaded: new Date(Number(objResponse.uploaded)),
    storageClass: multipartOptions?.storageClass ?? objResponse.storageClass,
    checksums: {},
    httpMetadata: {},
    customMetadata: multipartOptions?.customMetadata ?? {},
  };

  if (multipartOptions?.httpMetadata !== undefined) {
    result.httpMetadata =
      multipartOptions.httpMetadata instanceof Headers
        ? httpMetaObj
        : multipartOptions.httpMetadata;
  }

  switch (requestKey) {
    case 'httpMetadata':
      result.httpMetadata = httpMetaObj;
      break;
    case 'customMetadata':
      result.customMetadata = customMetadata;
      break;
    case 'classInfrequentAccess':
      result.storageClass = 'InfrequentAccess';
      break;
    case 'ssec':
    case 'ssecMultipart':
      result.ssecKeyMd5 = keyMd5;
      break;
    case 'multipleChecksums':
      result.checksums = {
        md5: md5Buffer.buffer,
        sha1: sha1Buffer.buffer,
        sha256: sha256Buffer.buffer,
      };
      break;
    case 'ranged':
      result.range = { offset: 10, length: 20 };
      break;
  }

  return result;
}

async function assertLargeRpcBody(requestKey, value, valueSize) {
  assert(value instanceof ReadableStream);
  assert.strictEqual(valueSize, largeRpcBodySize);
  const uploaded = new Uint8Array(await new Response(value).arrayBuffer());
  assert.strictEqual(uploaded.byteLength, largeRpcBodySize);

  switch (requestKey) {
    case 'largeBuffer':
      assert.strictEqual(uploaded[0], 0x11);
      assert.strictEqual(uploaded[1], 0x41);
      assert.strictEqual(uploaded.at(-2), 0x41);
      assert.strictEqual(uploaded.at(-1), 0x22);
      break;
    case 'largeString':
      assert.strictEqual(uploaded[0], 0xc3);
      assert.strictEqual(uploaded[1], 0xa9);
      assert.strictEqual(uploaded.at(-2), 0xc3);
      assert.strictEqual(uploaded.at(-1), 0xa9);
      break;
    case 'largeBlob':
      assert.strictEqual(uploaded[0], 0x5a);
      assert.strictEqual(uploaded.at(-1), 0x5a);
      break;
    default:
      assert.fail(`unexpected large RPC body key: ${requestKey}`);
  }
}

function buildGetResponse({ head, body, isList } = {}) {
  const encoder = new TextEncoder();
  let meta;
  if (!isList) {
    meta = {
      ...objResponse,
    };
  }
  meta = {
    ...meta,
    ...head,
  };
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

async function compareResponse(res, { head, body } = {}, bytes) {
  // Destructuring syntax looks ugly, but gets around needing to construct HeadResponse objects(somehow?)
  const { ...obj } = await res;
  obj.checksums = { ...obj.checksums };
  assert.deepEqual(obj, {
    ...HeadObject,
    ...head,
  });
  if (body) {
    if (bytes) {
      const expected = new Uint8Array(new TextEncoder().encode(body));
      const actual = await (await res).bytes();
      assert.equal(expected.byteLength, actual.byteLength);
      for (let i = 0; i < expected.byteLength; i++) {
        assert.equal(expected[i], actual[i]);
      }
      return;
    }
    assert.strictEqual(await (await res).text(), body);
  }
}

const testWorker = {
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
          // intentionally empty
        }

        // Assert it's the correct version
        assert((jsonRequest.version = 1));

        if (jsonRequest.method === 'delete') {
          if (jsonRequest.objects) {
            assert.deepEqual(jsonRequest.objects, [key, key + '2']);
          } else {
            assert.deepEqual(jsonRequest.object, key);
          }
          return new Response();
        }

        switch (jsonRequest.object) {
          case 'basicKey': {
            switch (jsonRequest.method) {
              case 'put': {
                break;
              }
              case 'createMultipartUpload': {
                return Response.json({
                  uploadId: 'multipartId',
                });
              }
              case 'uploadPart': {
                return Response.json({
                  etag: 'partEtag',
                });
              }
              case 'abortMultipartUpload': {
                return new Response();
              }
              case 'completeMultipartUpload': {
                return Response.json(objResponse);
              }
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
          case 'httpMetadata': {
            if (jsonRequest.method !== 'completeMultipartUpload') {
              assert.deepEqual(jsonRequest.httpFields, httpFields);
            }
            const head = {
              ...objResponse,
              httpFields,
            };
            switch (jsonRequest.method) {
              case 'put':
                return Response.json(head);
              case 'createMultipartUpload':
                return Response.json({
                  uploadId: 'multipartId',
                });
              case 'completeMultipartUpload': {
                return Response.json(head);
              }
            }
          }
          // falls through
          case 'customMetadata': {
            if (jsonRequest.method !== 'completeMultipartUpload') {
              assert.deepEqual(jsonRequest.customFields, customFields);
            }
            const head = {
              ...objResponse,
              customFields,
            };
            switch (jsonRequest.method) {
              case 'put':
                return Response.json(head);
              case 'createMultipartUpload':
                return Response.json({
                  uploadId: 'multipartId',
                });
              case 'completeMultipartUpload':
                return Response.json(head);
            }
          }
          // falls through
          case 'classDefault': {
            if (jsonRequest.method !== 'completeMultipartUpload') {
              assert.strictEqual(jsonRequest.storageClass, undefined);
            }
            const head = objResponse;
            switch (jsonRequest.method) {
              case 'put':
                return Response.json(head);
              case 'createMultipartUpload':
                return Response.json({
                  uploadId: 'multipartId',
                });
              case 'completeMultipartUpload':
                return Response.json(head);
            }
          }
          // falls through
          case 'classStandard': {
            if (jsonRequest.method !== 'completeMultipartUpload') {
              assert.deepEqual(jsonRequest.storageClass, 'Standard');
            }
            const head = {
              ...objResponse,
              storageClass: 'Standard',
            };
            switch (jsonRequest.method) {
              case 'put':
                return Response.json(head);
              case 'createMultipartUpload':
                return Response.json({
                  uploadId: 'multipartId',
                });
              case 'completeMultipartUpload':
                return Response.json(head);
            }
          }
          // falls through
          case 'classInfrequentAccess': {
            if (jsonRequest.method !== 'completeMultipartUpload') {
              assert.deepEqual(jsonRequest.storageClass, 'InfrequentAccess');
            }
            const head = {
              ...objResponse,
              storageClass: 'InfrequentAccess',
            };
            switch (jsonRequest.method) {
              case 'put':
                return Response.json(head);
              case 'createMultipartUpload':
                return Response.json({
                  uploadId: 'multipartId',
                });
              case 'completeMultipartUpload':
                return Response.json(head);
            }
          }
          // falls through
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
          case 'ssecMultipart': {
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
          // falls through
          case 'multipleChecksums': {
            const toHex = (buffer) =>
              Array.from(buffer, (b) => b.toString(16).padStart(2, '0')).join(
                ''
              );
            return Response.json({
              ...objResponse,
              checksums: {
                0: toHex(md5Buffer), // md5
                1: toHex(sha1Buffer), // sha1
                2: toHex(sha256Buffer), // sha256
              },
            });
          }
        }
        return Response.json(objResponse);
      }
      case 'GET': {
        const rawHeader = request.headers.get('cf-r2-request');
        const jsonRequest = JSON.parse(rawHeader);
        assert((jsonRequest.version = 1));
        if (jsonRequest.method === 'list') {
          switch (jsonRequest.prefix) {
            case 'basic': {
              assert.deepEqual(jsonRequest, {
                cursor: 'ai',
                delimiter: '/',
                include: [0, 1],
                limit: 1,
                method: 'list',
                newRuntime: true,
                prefix: 'basic',
                version: 1,
              });
              return buildGetResponse({
                head: {
                  objects: [objResponse],
                  truncated: true,
                  cursor: 'ai',
                  deliminatedPrefixes: [],
                },
                isList: true,
              });
            }
            case 'httpMeta': {
              assert.deepEqual(jsonRequest, {
                include: [0],
                method: 'list',
                newRuntime: true,
                prefix: 'httpMeta',
                version: 1,
              });

              return buildGetResponse({
                head: {
                  objects: [{ ...objResponse, httpFields, customFields: [] }],
                  truncated: false,
                  deliminatedPrefixes: [],
                },
                isList: true,
              });
            }
            case 'customMeta': {
              assert.deepEqual(jsonRequest, {
                include: [1],
                method: 'list',
                newRuntime: true,
                prefix: 'customMeta',
                version: 1,
              });

              return buildGetResponse({
                head: {
                  objects: [{ ...objResponse, httpFields: {}, customFields }],
                  truncated: false,
                  deliminatedPrefixes: [],
                },
                isList: true,
              });
            }
          }
        }
        assert(['get', 'head'].includes(jsonRequest.method));
        switch (jsonRequest.object) {
          case 'basicKey': {
            return buildGetResponse({ body });
          }
          case 'rangeOffLen': {
            assert.deepEqual(jsonRequest.range, {
              offset: '1',
              length: '3',
            });
            return buildGetResponse({
              head: {
                range: jsonRequest.range,
              },
              body: 'ont',
            });
          }
          case 'rangeSuff': {
            assert.deepEqual(jsonRequest.range, {
              suffix: '2',
            });
            return buildGetResponse({
              head: {
                range: {
                  offset: '6',
                  length: '2',
                },
              },
              body: 'nt',
            });
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
            return buildGetResponse({ body });
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
            return buildGetResponse({ body });
          }
          case 'httpMetadata': {
            const head = {
              httpFields,
            };
            switch (jsonRequest.method) {
              case 'head':
                return buildGetResponse({ head });
              case 'get':
                return buildGetResponse({ head, body });
            }
          }
          // falls through
          case 'customMetadata': {
            const head = {
              customFields,
            };
            switch (jsonRequest.method) {
              case 'head':
                return buildGetResponse({ head });
              case 'get':
                return buildGetResponse({ head, body });
            }
          }
          // falls through
          case 'classDefault':
          // falls through
          case 'classStandard': {
            const head = {
              storageClass: 'Standard',
            };
            switch (jsonRequest.method) {
              case 'head':
                return buildGetResponse({ head });
              case 'get':
                return buildGetResponse({ head, body });
            }
          }
          // falls through
          case 'classInfrequentAccess': {
            const head = {
              storageClass: 'InfrequentAccess',
            };
            switch (jsonRequest.method) {
              case 'head':
                return buildGetResponse({ head });
              case 'get':
                return buildGetResponse({ head, body });
            }
          }
          // falls through
          case 'ssec': {
            return buildGetResponse({
              head: {
                ssec: {
                  algorithm: 'aes256',
                  keyMd5,
                },
              },
              body,
            });
          }
          case 'multipleChecksums': {
            const toHex = (buffer) =>
              Array.from(buffer, (b) => b.toString(16).padStart(2, '0')).join(
                ''
              );
            return buildGetResponse({
              head: {
                checksums: {
                  0: toHex(md5Buffer), // md5
                  1: toHex(sha1Buffer), // sha1
                  2: toHex(sha256Buffer), // sha256
                },
              },
              body: jsonRequest.method === 'get' ? body : undefined,
            });
          }
        }
        throw new Error('Unexpected GET');
      }
      default:
        throw new Error('Unexpected HTTP Method');
    }
  },
  async test(ctrl, env, ctx) {
    // Basic Operations
    {
      // PutObject
      await compareResponse(env.BUCKET.put(key, body));
      // GetObject
      await compareResponse(env.BUCKET.get(key), {
        body,
      });
      // GetObject(.bytes())
      await compareResponse(env.BUCKET.get(key), { body }, true);
      // HeadObject
      const headObject = await env.BUCKET.head(key);
      await compareResponse(headObject);
      assert.strictEqual(typeof headObject.writeHttpMetadata, 'function');
      assert.strictEqual(typeof headObject.checksums.toJSON, 'function');
      // MultipartUploads
      {
        // CreateMultipartUpload
        const multi = await env.BUCKET.createMultipartUpload(key);
        assert.equal(multi.uploadId, 'multipartId');
        assert.equal(multi.key, key);
        // UploadPart
        const part = await multi.uploadPart(1, body);
        assert.equal(part.etag, 'partEtag');
        // Abort(doesn't quite make sense to abort **and** complete, but shouldn't matter)
        await multi.abort();
        // CompleteMultipartUpload
        await compareResponse(
          multi.complete([
            {
              partNumber: 1,
              etag: 'partEtag',
            },
          ])
        );
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
          truncated: true,
          cursor: 'ai',
          delimitedPrefixes: [],
        });
      }
      // DeleteObject
      {
        assert.strictEqual(await env.BUCKET.delete(key), undefined);
        assert.strictEqual(
          await env.BUCKET.delete([key, 'basicKey2']),
          undefined
        );
      }
    }
    // Ranged Reads
    {
      // Offset/Length
      {
        const range = {
          offset: 1,
          length: 3,
        };
        await compareResponse(
          env.BUCKET.get('rangeOffLen', {
            range,
          }),
          {
            head: { range },
          },
          'ont'
        );
      }
      // Suffix
      await compareResponse(
        env.BUCKET.get('rangeSuff', {
          range: {
            suffix: 2,
          },
        }),
        {
          head: {
            range: {
              offset: 6,
              length: 2,
            },
          },
        },
        'nt'
      );
    }
    // Conditionals
    {
      await assert.rejects(
        env.BUCKET.put('throwOnInvalidEtag', body, {
          onlyIf: new Headers({
            'if-match': 'strongEtag',
          }),
        }),
        { message: 'Invalid ETag in if-match header' }
      );
      await assert.rejects(
        env.BUCKET.put('throwOnInvalidEtag', body, {
          onlyIf: new Headers({
            'if-none-match': 'strongEtag',
          }),
        }),
        { message: 'Invalid ETag in if-none-match header' }
      );
      await env.BUCKET.put('onlyIfStrongEtag', body, {
        onlyIf: {
          etagMatches: 'strongEtag',
          etagDoesNotMatch: 'strongEtag',
          uploadedBefore: new Date('0'),
        },
      });
      await env.BUCKET.put('onlyIfWildcard', body, {
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
    // Metadata
    {
      // httpMetadata
      for (const httpMetadata of [httpMetaObj, httpMetaHeaders]) {
        const head = {
          httpMetadata: httpMetaObj,
        };
        // PutObject
        await compareResponse(
          env.BUCKET.put('httpMetadata', body, {
            httpMetadata,
          }),
          { head }
        );
        // HeadObject
        await compareResponse(env.BUCKET.head('httpMetadata'), { head });
        // GetObject
        {
          const objWithBody = await env.BUCKET.get('httpMetadata');
          await compareResponse(objWithBody, { head, body });
          // Hijacking this test to test `writeHttpMetadata` too
          const createdHeaders = new Headers();
          objWithBody.writeHttpMetadata(createdHeaders);
          assert.deepEqual(
            Object.fromEntries(createdHeaders.entries()),
            Object.fromEntries(httpMetaHeaders.entries())
          );
        }
        // ListObjects
        {
          const list = await env.BUCKET.list({
            prefix: 'httpMeta',
            include: ['httpMetadata'],
          });
          list.objects[0] = { ...list.objects[0] };
          list.objects[0].checksums = { ...list.objects[0].checksums };
          const expected = { ...HeadObject, ...head };
          if (
            env.R2_TRANSPORT === 'jsrpc' &&
            env.R2_LIST_HONOR_INCLUDE === 'true'
          ) {
            expected.customMetadata = undefined;
          }
          assert.deepEqual(list, {
            delimitedPrefixes: [],
            objects: [expected],
            truncated: false,
          });
        }
        // Multipart Upload
        await compareResponse(
          (
            await env.BUCKET.createMultipartUpload('httpMetadata', {
              httpMetadata,
            })
          ).complete([]),
          { head }
        );
      }
      // customMetadata
      {
        // PutObject
        const head = {
          customMetadata,
        };
        await compareResponse(
          env.BUCKET.put('customMetadata', body, {
            customMetadata,
          }),
          { head }
        );
        // HeadObject
        await compareResponse(await env.BUCKET.head('customMetadata'), {
          head,
        });
        // GetObject
        await compareResponse(await env.BUCKET.get('customMetadata'), {
          head,
          body,
        });
        // ListObjects
        {
          const list = await env.BUCKET.list({
            prefix: 'customMeta',
            include: ['customMetadata'],
          });
          list.objects[0] = { ...list.objects[0] };
          list.objects[0].checksums = { ...list.objects[0].checksums };
          const expected = { ...HeadObject, ...head };
          if (
            env.R2_TRANSPORT === 'jsrpc' &&
            env.R2_LIST_HONOR_INCLUDE === 'true'
          ) {
            expected.httpMetadata = undefined;
          }
          assert.deepEqual(list, {
            delimitedPrefixes: [],
            objects: [expected],
            truncated: false,
          });
        }
        // Multipart Upload
        await compareResponse(
          (
            await env.BUCKET.createMultipartUpload('customMetadata', {
              customMetadata,
            })
          ).complete([]),
          { head }
        );
      }
    }
    // StorageClasses
    {
      for (const storageClassName of [
        'Default',
        'Standard',
        'InfrequentAccess',
      ]) {
        const key = 'class' + storageClassName;
        const storageClass =
          storageClassName === 'Default' ? undefined : storageClassName;
        const head = {
          storageClass:
            storageClassName === 'Default' ? 'Standard' : storageClassName,
        };
        // PutObject
        await compareResponse(
          env.BUCKET.put(key, body, {
            storageClass,
          }),
          { head }
        );
        // HeadObject
        await compareResponse(env.BUCKET.head(key), { head });
        // GetObject
        await compareResponse(env.BUCKET.get(key), { head, body });
        // Multipart Upload
        await compareResponse(
          (
            await env.BUCKET.createMultipartUpload(key, {
              storageClass,
            })
          ).complete([]),
          { head }
        );
      }
    }
    // SSEC
    {
      const head = {
        ssecKeyMd5: keyMd5,
      };
      for (const ssecKey of [bufferKey, hexKey]) {
        // PutObject
        await compareResponse(
          env.BUCKET.put('ssec', body, {
            ssecKey,
          }),
          { head }
        );
        // GetObject
        await compareResponse(
          env.BUCKET.get('ssec', {
            ssecKey,
          }),
          { head, body }
        );
        // HeadObject
        await compareResponse(
          env.BUCKET.head('ssec', {
            ssecKey,
          }),
          { head }
        );
        // MultipartUpload
        {
          // CreateMultipartUpload
          const multi = await env.BUCKET.createMultipartUpload(
            'ssecMultipart',
            {
              ssecKey,
            }
          );
          assert.equal(multi.uploadId, 'multipartId');
          // UploadPart
          const part = await multi.uploadPart(1, 'hey', {
            ssecKey,
          });
          assert.equal(part.etag, 'partEtag');
          // CompleteMultipartUpload
          await compareResponse(
            multi.complete([
              {
                partNumber: 1,
                etag: 'partEtag',
              },
            ]),
            { head }
          );
        }
      }
    }
    // Checksums
    {
      // This tests the instrumentation with multiple checksums to ensure proper tag handling
      let resp = await env.BUCKET.put('multipleChecksums', body, {
        md5: md5Buffer,
      });
      assert.ok(resp);

      // Also test HEAD operation to verify checksum tags
      const headResp = await env.BUCKET.head('multipleChecksums');
      assert.deepStrictEqual(new Uint8Array(headResp.checksums.md5), md5Buffer);
      assert.deepStrictEqual(
        new Uint8Array(headResp.checksums.sha1),
        sha1Buffer
      );
      assert.deepStrictEqual(
        new Uint8Array(headResp.checksums.sha256),
        sha256Buffer
      );
      assert.deepStrictEqual(headResp.checksums.toJSON(), {
        md5: '9a0364b9e99bb480dd25e1f0284c8555',
        sha1: '2a0364b9e99bb480dd25e1f0284c855511223344',
        sha256:
          '3a0364b9e99bb480dd25e1f0284c8555112233445566778899aabbccddeeff00',
      });
    }
  },
};

// The production gateway supports HTTP and named RPC on the same entrypoint. Keeping both here is
// also necessary while operations are migrated incrementally: methods without an RPC implementation
// continue to use fetch() even when the JSRPC compatibility flag is enabled.
export class R2BindingEntrypoint extends WorkerEntrypoint {
  fetch(request) {
    const encodedRequest = request.headers.get('cf-r2-request');
    if (
      encodedRequest !== null &&
      ['get', 'list'].includes(JSON.parse(encodedRequest).method)
    ) {
      throw new Error('get and list must use JSRPC');
    }
    return testWorker.fetch(request, this.env, this.ctx);
  }

  head(requestKey) {
    if (requestKey === 'missing') {
      return null;
    }
    if (requestKey === 'boom') {
      throw new Error('head: no such bucket (10006)');
    }

    const result = buildRpcHead(requestKey);
    if (requestKey === 'rpc-malformed-size') {
      result.size = -1;
    }
    if (requestKey === 'rpc-malformed-range') {
      result.range = { offset: -1, length: 1 };
    }
    if (requestKey === 'rpc-malformed-date') {
      result.uploaded = new Date(NaN);
    }
    return result;
  }

  get(requestKey, options) {
    if (requestKey === 'missing') {
      return null;
    }
    if (requestKey === 'rpc-get-boom') {
      throw new Error('get: no such bucket (10006)');
    }
    if (requestKey === 'rpc-malformed-kind') {
      return { kind: 'other', object: buildRpcHead(requestKey) };
    }
    if (requestKey === 'rpc-malformed-metadata-body') {
      return {
        kind: 'metadata',
        object: buildRpcHead(requestKey),
        body: new ReadableStream(),
      };
    }
    if (requestKey === 'rpc-malformed-missing-body') {
      return { kind: 'body', object: buildRpcHead(requestKey) };
    }
    if (requestKey === 'rpc-options') {
      assert.deepStrictEqual(options, {
        onlyIf: {
          etagMatches: 'strongEtag',
          uploadedBefore: new Date(0),
          secondsGranularity: false,
        },
        range: { offset: 1, length: 3 },
        ssecKey: hexKey,
      });
    }
    if (requestKey === 'rpc-header-options') {
      assert.deepStrictEqual(options, {
        onlyIf: {
          etagMatches: 'strongEtag',
          uploadedAfter: new Date(0),
          secondsGranularity: true,
        },
        range: 'bytes=1-3',
      });
    }
    if (requestKey === 'rpc-must-not-call') {
      throw new Error('get RPC method must not be called');
    }

    const object = buildRpcHead(requestKey);
    if (requestKey === 'rangeOffLen') {
      assert.deepStrictEqual(options, { range: { offset: 1, length: 3 } });
      object.range = { offset: 1, length: 3 };
    } else if (requestKey === 'rangeSuff') {
      assert.deepStrictEqual(options, { range: { suffix: 2 } });
      object.range = { offset: 6, length: 2 };
    }

    if (requestKey === 'rpc-conditional-metadata') {
      return { kind: 'metadata', object };
    }

    const responseBody =
      requestKey === 'rangeOffLen'
        ? 'ont'
        : requestKey === 'rangeSuff'
          ? 'nt'
          : requestKey === 'rpc-json'
            ? JSON.stringify({ ok: true })
            : body;
    let sent = false;
    return {
      kind: 'body',
      object,
      body: new ReadableStream({
        pull(controller) {
          assert.strictEqual(sent, false);
          sent = true;
          controller.enqueue(new TextEncoder().encode(responseBody));
          controller.close();
        },
      }),
    };
  }

  delete(keys) {
    if (keys === 'boom') {
      throw new Error('delete: bad keys (10021)');
    }
    if (Array.isArray(keys)) {
      assert.deepEqual(keys, [key, key + '2']);
    } else {
      assert.strictEqual(keys, key);
    }
  }

  async put(requestKey, value, options, valueSize) {
    if (requestKey === 'rpc-must-not-call') {
      throw new Error('put RPC method must not be called');
    }
    if (requestKey === 'rpc-null-value') {
      assert.strictEqual(value, null);
      assert.strictEqual(valueSize, 0);
      return buildRpcHead(requestKey);
    }
    if (requestKey === 'rpc-conditional-null') {
      assert.deepStrictEqual(options.onlyIf, {
        etagMatches: 'strongEtag',
        secondsGranularity: false,
      });
      return null;
    }
    if (requestKey === 'rpc-put-options') {
      assert.deepStrictEqual(options.onlyIf, {
        etagMatches: 'strongEtag',
        uploadedBefore: new Date(0),
        secondsGranularity: false,
      });
      assert.deepStrictEqual(options.httpMetadata, httpMetaObj);
      assert.deepStrictEqual(options.customMetadata, customMetadata);
      assert.deepStrictEqual(new Uint8Array(options.md5), md5Buffer);
      assert.strictEqual(options.storageClass, 'InfrequentAccess');
      assert.strictEqual(options.ssecKey, hexKey);
    }
    if (requestKey.startsWith('large')) {
      await assertLargeRpcBody(requestKey, value, valueSize);
      return buildRpcHead(requestKey, options);
    }
    const uploaded = await new Response(value).text();
    const expected = requestKey === 'rpcStream' ? rpcStreamBody : body;
    assert.strictEqual(uploaded, expected);
    assert.strictEqual(
      valueSize,
      new TextEncoder().encode(expected).byteLength
    );
    return buildRpcHead(requestKey, options);
  }

  list(options) {
    const honorsIncludes = this.env.R2_LIST_HONOR_INCLUDE === 'true';
    const effectiveIncludes = honorsIncludes
      ? (options?.include ?? [])
      : ['httpMetadata', 'customMetadata'];

    if (options?.prefix === 'rpc-boom') {
      throw new Error('list: no such bucket (10006)');
    }
    if (options?.prefix === 'rpc-malformed') {
      return { objects: [{}], truncated: false };
    }
    if (options?.prefix === 'rpc-options') {
      assert.deepStrictEqual(options, {
        limit: 2,
        prefix: 'rpc-options',
        cursor: 'cursor-in',
        delimiter: '/',
        startAfter: 'after',
        include: effectiveIncludes,
      });
      return {
        objects: [],
        truncated: true,
        cursor: 'cursor-out',
        delimitedPrefixes: ['rpc-options/'],
      };
    }

    if (options?.prefix === 'basic') {
      assert.deepStrictEqual(options, {
        limit: 1,
        prefix: 'basic',
        cursor: 'ai',
        delimiter: '/',
        include: effectiveIncludes,
      });
      return {
        objects: [listRpcHead('basic', effectiveIncludes)],
        truncated: true,
        cursor: 'ai',
      };
    }
    if (options?.prefix === 'httpMeta') {
      return {
        objects: [listRpcHead('httpMetadata', effectiveIncludes)],
        truncated: false,
      };
    }
    if (options?.prefix === 'customMeta') {
      return {
        objects: [listRpcHead('customMetadata', effectiveIncludes)],
        truncated: false,
      };
    }
    if (options?.prefix === 'rpc-metadata') {
      return {
        objects: [listRpcHead('basic', effectiveIncludes)],
        truncated: false,
      };
    }

    if (honorsIncludes) {
      assert.strictEqual(options, undefined);
    } else {
      assert.deepStrictEqual(options, {
        include: ['httpMetadata', 'customMetadata'],
      });
    }
    return { objects: [], truncated: false };
  }

  createMultipartUpload(requestKey, options) {
    if (requestKey === 'rpc-must-not-call') {
      throw new Error('createMultipartUpload RPC method must not be called');
    }
    if (requestKey === 'rpc-create-options') {
      assert.deepStrictEqual(options.httpMetadata, httpMetaObj);
      assert.deepStrictEqual(options.customMetadata, customMetadata);
      assert.strictEqual(options.storageClass, 'InfrequentAccess');
      assert.strictEqual(options.ssecKey, hexKey);
    }
    assert.strictEqual(typeof requestKey, 'string');
    return new MultipartUploadTarget(requestKey, 'multipartId', options);
  }

  resumeMultipartUpload(requestKey, uploadId) {
    assert.strictEqual(typeof requestKey, 'string');
    assert.strictEqual(typeof uploadId, 'string');
    return new MultipartUploadTarget(requestKey, uploadId);
  }
}

function listRpcHead(requestKey, includes) {
  const result = buildRpcHead(requestKey);
  if (!includes.includes('httpMetadata')) {
    delete result.httpMetadata;
  }
  if (!includes.includes('customMetadata')) {
    delete result.customMetadata;
  }
  return result;
}

class MultipartUploadTarget extends RpcTarget {
  #key;
  #uploadId;
  #options;
  #aborted = false;

  constructor(requestKey, uploadId, options) {
    super();
    this.#key = requestKey;
    this.#uploadId = uploadId;
    this.#options = options;
  }

  getUploadId() {
    return this.#uploadId;
  }

  async uploadPart(partNumber, value, options, valueSize) {
    assert(partNumber >= 1 && partNumber <= 10000);
    if (this.#key === 'rpc-must-not-call') {
      throw new Error('uploadPart RPC method must not be called');
    }
    if (this.#key === 'largeBuffer') {
      await assertLargeRpcBody(this.#key, value, valueSize);
      return { partNumber, etag: 'partEtag' };
    }
    const uploaded = await new Response(value).text();
    const expected =
      this.#key === 'ssecMultipart'
        ? 'hey'
        : this.#key === 'rpcStream'
          ? rpcStreamBody
          : body;
    assert.strictEqual(uploaded, expected);
    assert.strictEqual(
      valueSize,
      new TextEncoder().encode(expected).byteLength
    );
    if (this.#key === 'ssecMultipart') {
      assert.strictEqual(options?.ssecKey, hexKey);
    }
    return {
      partNumber: this.#key === 'rpc-wrong-part-number' ? 9999 : partNumber,
      etag: this.#uploadId === 'resumedId' ? 'resumedRpcPartEtag' : 'partEtag',
    };
  }

  abort() {
    this.#aborted = true;
  }

  complete(uploadedParts) {
    if (this.#uploadId === 'resumedId') {
      assert.strictEqual(this.#aborted, true);
    }
    for (const part of uploadedParts) {
      assert(part.partNumber >= 1 && part.partNumber <= 10000);
      assert.strictEqual(typeof part.etag, 'string');
    }
    const result = buildRpcHead(this.#key, this.#options);
    if (this.#uploadId === 'resumedId') {
      result.version = 'resumedRpcObjectVersion';
    }
    return result;
  }
}

// These cases cover RPC boundary behavior that the HTTP-oriented fake cannot observe directly.
// The canonical API suite remains identical for both transport configurations.
export const jsrpcTransportTests = {
  async test(ctrl, env, ctx) {
    if (env.R2_TRANSPORT !== 'jsrpc') {
      return;
    }

    assert.strictEqual(await env.BUCKET.head('missing'), null);

    const ranged = await env.BUCKET.head('ranged');
    assert.deepStrictEqual(ranged.range, { offset: 10, length: 20 });

    await assert.rejects(env.BUCKET.head('boom'), (err) => {
      assert.strictEqual(err.message, 'head: no such bucket (10006)');
      assert.strictEqual(err.code, undefined);
      return true;
    });
    await assert.rejects(env.BUCKET.head('rpc-malformed-size'), {
      message: 'Malformed R2 RPC result: size must be a non-negative integer.',
    });
    await assert.rejects(env.BUCKET.head('rpc-malformed-range'), {
      message:
        'Malformed R2 RPC result: range offset must be a non-negative integer.',
    });
    await assert.rejects(env.BUCKET.head('rpc-malformed-date'), {
      message: 'The value cannot be converted because it is not a valid Date.',
    });

    await assert.rejects(env.BUCKET.delete('boom'), {
      message: 'delete: bad keys (10021)',
    });

    const coerced = await env.BUCKET.head(12345);
    assert.strictEqual(coerced.key, key);

    assert.strictEqual(await env.BUCKET.get('missing'), null);

    const conditional = await env.BUCKET.get('rpc-conditional-metadata', {
      onlyIf: { etagMatches: 'objectEtag' },
    });
    assert.strictEqual(conditional.body, undefined);
    assert.deepStrictEqual(conditional.httpMetadata, {});
    assert.deepStrictEqual(conditional.customMetadata, {});
    assert.strictEqual(typeof conditional.writeHttpMetadata, 'function');

    const lazyBody = await env.BUCKET.get('rpc-lazy-body');
    assert.strictEqual(lazyBody.bodyUsed, false);
    assert(lazyBody.body instanceof ReadableStream);
    assert.strictEqual(await lazyBody.text(), body);
    assert.strictEqual(lazyBody.bodyUsed, true);

    const bytesResult = await env.BUCKET.get('rpc-bytes');
    assert.deepStrictEqual(
      await bytesResult.bytes(),
      new TextEncoder().encode(body)
    );
    assert.strictEqual(bytesResult.bodyUsed, true);
    const bufferResult = await env.BUCKET.get('rpc-array-buffer');
    assert.deepStrictEqual(
      new Uint8Array(await bufferResult.arrayBuffer()),
      new TextEncoder().encode(body)
    );
    const jsonResult = await env.BUCKET.get('rpc-json');
    assert.deepStrictEqual(await jsonResult.json(), { ok: true });
    const blobResult = await env.BUCKET.get('rpc-blob');
    assert.strictEqual(await (await blobResult.blob()).text(), body);

    await env.BUCKET.get('rpc-options', {
      onlyIf: {
        etagMatches: 'strongEtag',
        uploadedBefore: new Date(0),
      },
      range: { offset: 1, length: 3 },
      ssecKey: bufferKey,
    });
    await env.BUCKET.get('rpc-header-options', {
      onlyIf: new Headers({
        'if-match': '"strongEtag"',
        'if-modified-since': new Date(0).toUTCString(),
      }),
      range: new Headers({ range: 'bytes=1-3' }),
    });

    await assert.rejects(
      env.BUCKET.get('rpc-must-not-call', { range: { offset: -1 } }),
      {
        message:
          'Invalid range. Starting offset (-1) must be greater than or equal to 0.',
      }
    );
    await assert.rejects(
      env.BUCKET.get('rpc-must-not-call', { range: { suffix: 1, length: 1 } }),
      { message: 'Suffix is incompatible with length.' }
    );
    await assert.rejects(
      env.BUCKET.get('rpc-must-not-call', { ssecKey: 'bad' }),
      { message: 'SSE-C Key must be 32 bytes in length' }
    );
    await assert.rejects(
      env.BUCKET.get('rpc-must-not-call', {
        onlyIf: { etagMatches: '"quoted"' },
      }),
      {
        message: 'Conditional ETag should not be wrapped in quotes ("quoted").',
      }
    );

    await assert.rejects(env.BUCKET.get('rpc-malformed-kind'), {
      message: 'Malformed R2 get RPC result: unknown result kind other.',
    });
    await assert.rejects(env.BUCKET.get('rpc-malformed-metadata-body'), {
      message: 'Malformed R2 get RPC result: metadata result had a body.',
    });
    await assert.rejects(env.BUCKET.get('rpc-malformed-missing-body'), {
      message: 'Malformed R2 get RPC result: body result did not have a body.',
    });
    await assert.rejects(env.BUCKET.get('rpc-get-boom'), {
      message: 'get: no such bucket (10006)',
    });

    const emptyList = await env.BUCKET.list();
    assert.deepStrictEqual(emptyList, {
      objects: [],
      truncated: false,
      delimitedPrefixes: [],
    });

    const optionList = await env.BUCKET.list({
      limit: 2,
      prefix: 'rpc-options',
      cursor: 'cursor-in',
      delimiter: '/',
      startAfter: 'after',
      include: [],
    });
    assert.deepStrictEqual(optionList, {
      objects: [],
      truncated: true,
      cursor: 'cursor-out',
      delimitedPrefixes: ['rpc-options/'],
    });

    if (env.R2_LIST_HONOR_INCLUDE === 'true') {
      for (const include of [
        [],
        ['httpMetadata'],
        ['customMetadata'],
        ['httpMetadata', 'customMetadata'],
      ]) {
        const listed = await env.BUCKET.list({
          prefix: 'rpc-metadata',
          include,
        });
        const object = listed.objects[0];
        assert.strictEqual(typeof object.writeHttpMetadata, 'function');
        assert.strictEqual(typeof object.checksums.toJSON, 'function');
        assert.strictEqual(
          object.httpMetadata === undefined,
          !include.includes('httpMetadata')
        );
        assert.strictEqual(
          object.customMetadata === undefined,
          !include.includes('customMetadata')
        );
        if (include.includes('httpMetadata')) {
          assert.deepStrictEqual(object.httpMetadata, {});
        }
        if (include.includes('customMetadata')) {
          assert.deepStrictEqual(object.customMetadata, {});
        }
      }
    }

    await assert.rejects(env.BUCKET.list({ prefix: 'rpc-boom' }), {
      message: 'list: no such bucket (10006)',
    });
    await assert.rejects(env.BUCKET.list({ prefix: 'rpc-malformed' }));

    const nullPut = await env.BUCKET.put('rpc-null-value', null);
    assert.strictEqual(nullPut.key, key);
    assert.strictEqual(
      await env.BUCKET.put('rpc-conditional-null', body, {
        onlyIf: { etagMatches: 'strongEtag' },
      }),
      null
    );
    await env.BUCKET.put('rpc-put-options', body, {
      onlyIf: {
        etagMatches: 'strongEtag',
        uploadedBefore: new Date(0),
      },
      httpMetadata: httpMetaHeaders,
      customMetadata,
      md5: md5Buffer,
      storageClass: 'InfrequentAccess',
      ssecKey: bufferKey,
    });

    await assert.rejects(
      env.BUCKET.put('rpc-must-not-call', body, { md5: new Uint8Array(1) }),
      { message: 'MD5 is 16 bytes, not 1' }
    );
    await assert.rejects(
      env.BUCKET.put('rpc-must-not-call', body, {
        md5: 'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz',
      }),
      { message: "Provided MD5 wasn't a valid hex string" }
    );
    await assert.rejects(
      env.BUCKET.put('rpc-must-not-call', body, {
        md5: md5Buffer,
        sha1: sha1Buffer,
      }),
      { message: 'You cannot specify multiple hashing algorithms.' }
    );
    await assert.rejects(
      env.BUCKET.createMultipartUpload('rpc-must-not-call', {
        ssecKey: 'bad',
      }),
      { message: 'SSE-C Key must be 32 bytes in length' }
    );
    const invalidPartUpload = env.BUCKET.resumeMultipartUpload(
      'rpc-must-not-call',
      'invalidPartUploadId'
    );
    await assert.rejects(
      invalidPartUpload.uploadPart(1, body, { ssecKey: 'bad' }),
      { message: 'SSE-C Key must be 32 bytes in length' }
    );

    const createOptionsUpload = await env.BUCKET.createMultipartUpload(
      'rpc-create-options',
      {
        httpMetadata: httpMetaHeaders,
        customMetadata,
        storageClass: 'InfrequentAccess',
        ssecKey: bufferKey,
      }
    );
    assert.strictEqual(createOptionsUpload.uploadId, 'multipartId');

    const encodedStreamBody = new TextEncoder().encode(rpcStreamBody);
    {
      const { readable, writable } = new FixedLengthStream(
        encodedStreamBody.byteLength
      );
      const writer = writable.getWriter();
      const writing = writer
        .write(encodedStreamBody)
        .then(() => writer.close());
      const result = await env.BUCKET.put('rpcStream', readable);
      await writing;
      assert.strictEqual(result.size, Number(objResponse.size));
    }

    let unknownLengthCancelReason;
    await assert.rejects(
      env.BUCKET.put(
        'unknownLengthStream',
        new ReadableStream({
          cancel(reason) {
            unknownLengthCancelReason = reason;
          },
        })
      ),
      {
        message:
          'Provided readable stream must have a known length (request/response body or readable half of FixedLengthStream)',
      }
    );
    assert.strictEqual(
      unknownLengthCancelReason.message,
      'Stream cancelled because the associated put operation encountered an error.'
    );

    const invalidOptionsStream = new FixedLengthStream(1);
    const invalidOptionsWriter = invalidOptionsStream.writable.getWriter();
    const invalidOptionsWriting = invalidOptionsWriter.write(
      new Uint8Array([1])
    );
    await assert.rejects(
      env.BUCKET.put('rpc-must-not-call', invalidOptionsStream.readable, {
        onlyIf: { etagMatches: '"quoted"' },
      }),
      {
        message: 'Conditional ETag should not be wrapped in quotes ("quoted").',
      }
    );
    await assert.rejects(invalidOptionsWriting, {
      message:
        'Stream cancelled because the associated put operation encountered an error.',
    });

    const largeBufferBacking = new Uint8Array(largeRpcBodySize + 2);
    largeBufferBacking.fill(0x41);
    const largeBuffer = largeBufferBacking.subarray(1, -1);
    largeBuffer[0] = 0x11;
    largeBuffer[largeBuffer.length - 1] = 0x22;
    await env.BUCKET.put('largeBuffer', largeBuffer);

    const largeString = 'é'.repeat(largeRpcBodySize / 2);
    await env.BUCKET.put('largeString', largeString);

    const largeBlobBytes = new Uint8Array(largeRpcBodySize);
    largeBlobBytes.fill(0x5a);
    await env.BUCKET.put('largeBlob', new Blob([largeBlobBytes]));

    const resumed = env.BUCKET.resumeMultipartUpload(key, 'resumedId');
    assert.strictEqual(resumed.key, key);
    assert.strictEqual(resumed.uploadId, 'resumedId');
    const resumedPart = await resumed.uploadPart(1, body);
    assert.deepStrictEqual(resumedPart, {
      partNumber: 1,
      etag: 'resumedRpcPartEtag',
    });
    await resumed.abort();
    const resumedObject = await resumed.complete([resumedPart]);
    assert.strictEqual(resumedObject.key, key);
    assert.strictEqual(resumedObject.version, 'resumedRpcObjectVersion');
    assert.strictEqual(resumedObject.etag, objResponse.etag);
    assert.strictEqual(resumedObject.httpEtag, `"${objResponse.etag}"`);
    assert.strictEqual(resumedObject.size, Number(objResponse.size));
    assert(resumedObject.uploaded instanceof Date);
    assert.strictEqual(resumedObject.storageClass, objResponse.storageClass);
    const resumedHeaders = new Headers();
    resumedObject.writeHttpMetadata(resumedHeaders);
    assert.deepStrictEqual([...resumedHeaders], []);
    assert.strictEqual(typeof resumedObject.checksums.toJSON, 'function');
    assert.deepStrictEqual(resumedObject.checksums.toJSON(), {});

    const streamedUpload = env.BUCKET.resumeMultipartUpload(
      'rpcStream',
      'streamedId'
    );
    const { readable, writable } = new FixedLengthStream(
      encodedStreamBody.byteLength
    );
    const writer = writable.getWriter();
    const writing = writer.write(encodedStreamBody).then(() => writer.close());
    const streamedPart = await streamedUpload.uploadPart(2, readable);
    await writing;
    assert.deepStrictEqual(streamedPart, {
      partNumber: 2,
      etag: 'partEtag',
    });

    const largeUpload = env.BUCKET.resumeMultipartUpload(
      'largeBuffer',
      'largeUploadId'
    );
    assert.deepStrictEqual(await largeUpload.uploadPart(1, largeBuffer), {
      partNumber: 1,
      etag: 'partEtag',
    });

    const wrongPartUpload = env.BUCKET.resumeMultipartUpload(
      'rpc-wrong-part-number',
      'wrongPartUploadId'
    );
    assert.deepStrictEqual(await wrongPartUpload.uploadPart(3, body), {
      partNumber: 3,
      etag: 'partEtag',
    });
  },
};

export default testWorker;
