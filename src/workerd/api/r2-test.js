// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

const key = 'basicKey';
const body = 'content';
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
async function compareResponse(res, { head, body } = {}) {
  // Destructuring syntax looks ugly, but gets around needing to construct HeadResponse objects(somehow?)
  const { ...obj } = await res;
  obj.checksums = { ...obj.checksums };
  assert.deepEqual(obj, {
    ...HeadObject,
    ...head,
  });
  if (body) {
    assert.strictEqual(await (await res).text(), body);
  }
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
          case 'classDefault':
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
      // HeadObject
      await compareResponse(env.BUCKET.head(key));
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
        await env.BUCKET.delete(key);
        await env.BUCKET.delete([key, 'basicKey2']);
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
          assert.deepEqual(list, {
            delimitedPrefixes: [],
            objects: [{ ...HeadObject, ...head }],
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
          assert.deepEqual(list, {
            delimitedPrefixes: [],
            objects: [{ ...HeadObject, ...head }],
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
  },
};
