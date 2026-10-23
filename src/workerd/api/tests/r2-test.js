// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { Buffer } from 'node:buffer';
import { WorkerEntrypoint } from 'cloudflare:workers';

const key = 'basicKey';
const body = 'content';
const rpcStreamBody = 'café';
const stringUploadCases = {
  'string-upload-below-limit': { size: (8 << 20) - 1, suffix: 'Ā' },
  'string-upload-at-limit': { size: 8 << 20, suffix: 'Ā' },
  'string-upload-above-limit': { size: (8 << 20) + 1, suffix: 'Ā' },
  'string-upload-rpc-limit': { size: 16 << 20, suffix: 'Ā' },
  'string-upload-ascii': { size: 16 << 20, suffix: '' },
};
const perKeyErrorDeleteKeys = ['existing', 'missing'];
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
  size: '7',
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
  size: 7,
  version: 'objectVersion',
  key,
};
// The HTTP protocol reports failures with a status code and a CF-R2-Error header, which the
// binding turns into an Error whose message is "<action>: <message> (<v4code>)".
function r2ErrorHeaders({ v4code, message }) {
  return { 'cf-r2-error': JSON.stringify({ version: 1, v4code, message }) };
}

function r2ErrorResponse(error) {
  return new Response(null, {
    status: error.status,
    headers: r2ErrorHeaders(error),
  });
}

const noSuchKeyError = {
  status: 404,
  v4code: 10007,
  message: 'The specified key does not exist.',
};
const noSuchBucketError = {
  status: 404,
  v4code: 10006,
  message: 'no such bucket',
};
const preconditionFailedError = {
  status: 412,
  v4code: 10031,
  message: 'At least one of the pre-conditions you specified did not hold.',
};
const uploadNotFoundError = {
  status: 404,
  v4code: 10024,
  message: 'Upload not found',
};

// Multipart uploads for keys starting with 'rpc-multipart-' are tracked so that the mock can
// report uploads that were never created, or were already completed or aborted. The first upload
// for a key gets the ID '<key>-id', and later uploads for the same key get '<key>-id-<n>'.
const activeMultipartUploads = new Map();
const multipartUploadCounts = new Map();

function handleTrackedMultipartRequest(jsonRequest, uploadedBody) {
  const requestKey = jsonRequest.object;
  const { uploadId } = jsonRequest;
  const isActive = activeMultipartUploads.get(uploadId) === requestKey;
  switch (jsonRequest.method) {
    case 'createMultipartUpload': {
      // Metadata is captured when createMultipartUpload() is called, so later changes to the
      // caller's objects are not sent.
      if (jsonRequest.customFields !== undefined) {
        assert.deepStrictEqual(jsonRequest.customFields, [
          { k: 'uploadKey', v: requestKey },
        ]);
        assert.deepStrictEqual(jsonRequest.httpFields, httpFields);
      }
      const count = (multipartUploadCounts.get(requestKey) ?? 0) + 1;
      multipartUploadCounts.set(requestKey, count);
      const newUploadId =
        count === 1 ? `${requestKey}-id` : `${requestKey}-id-${count}`;
      activeMultipartUploads.set(newUploadId, requestKey);
      return Response.json({ uploadId: newUploadId });
    }
    case 'uploadPart':
      if (!isActive) {
        return r2ErrorResponse(uploadNotFoundError);
      }
      assert.strictEqual(uploadedBody, body);
      return Response.json({
        etag: `${requestKey}/${uploadId}/${jsonRequest.partNumber}`,
      });
    case 'completeMultipartUpload':
      if (!isActive) {
        return r2ErrorResponse(uploadNotFoundError);
      }
      for (const { part, etag } of jsonRequest.parts ?? []) {
        assert.strictEqual(etag, `${requestKey}/${uploadId}/${part}`);
      }
      activeMultipartUploads.delete(uploadId);
      return Response.json({
        ...objResponse,
        name: requestKey,
        version: uploadId,
      });
    case 'abortMultipartUpload':
      // Aborting an upload that no longer exists succeeds.
      if (isActive) {
        activeMultipartUploads.delete(uploadId);
      }
      return new Response();
  }
  throw new Error(`Unexpected method: ${jsonRequest.method}`);
}

async function handleStringUploadRequest(request, jsonRequest, metadataSize) {
  const expected = stringUploadCases[jsonRequest.object];
  const suffix = new TextEncoder().encode(expected.suffix);
  const asciiLength = expected.size - suffix.byteLength;
  assert.strictEqual(
    request.headers.get('content-length'),
    String(metadataSize + expected.size)
  );

  // Check every byte without retaining another full copy of the large upload.
  const ascii = new Uint8Array(1 << 20).fill(0x61);
  const reader = request.body.getReader({ mode: 'byob' });
  let received = 0;
  try {
    for (;;) {
      const { value: chunk, done } = await reader.readAtLeast(
        ascii.length,
        new Uint8Array(ascii.length)
      );
      if (done) break;
      const asciiEnd = Math.min(
        chunk.length,
        Math.max(asciiLength - received, 0)
      );
      assert.strictEqual(
        Buffer.compare(
          chunk.subarray(0, asciiEnd),
          ascii.subarray(0, asciiEnd)
        ),
        0
      );
      const suffixStart = Math.max(received - asciiLength, 0);
      assert.deepStrictEqual(
        chunk.subarray(asciiEnd),
        suffix.subarray(suffixStart, suffixStart + chunk.length - asciiEnd)
      );
      received += chunk.length;
    }
  } finally {
    reader.releaseLock();
  }
  assert.strictEqual(received, expected.size);

  if (jsonRequest.method === 'put') {
    return Response.json({
      ...objResponse,
      name: jsonRequest.object,
      size: String(received),
    });
  }
  assert.strictEqual(jsonRequest.method, 'uploadPart');
  assert.strictEqual(jsonRequest.uploadId, 'string-upload-id');
  assert.strictEqual(jsonRequest.partNumber, 1);
  return Response.json({ etag: `string-upload-${received}` });
}

// An R2 error makes this an error response, such as a failed precondition that still carries the
// object's metadata but no body.
function buildGetResponse({ head, body, isList, error } = {}) {
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
  const headers = {
    'cf-r2-metadata-size': metadata.length.toString(),
    'content-length': metadata.length.toString(),
  };
  if (error === undefined) {
    return new Response(responseBody, { headers });
  }
  return new Response(responseBody, {
    status: error.status,
    headers: { ...headers, ...r2ErrorHeaders(error) },
  });
}

// With the fastJsgStruct compatibility flag, a JSG struct such as R2HTTPMetadata or R2Range has
// every optional field as an own property, set to undefined when the field is unset. Without the
// flag, unset fields are absent. Dropping unset fields lets an assertion check the same shape in
// both modes.
function withoutUnsetFields(struct) {
  return Object.fromEntries(
    Object.entries(struct).filter(([, value]) => value !== undefined)
  );
}

// Copies a list() result into plain objects that compare the same in both JSG struct modes.
function plainListResult(list) {
  return {
    ...withoutUnsetFields(list),
    objects: list.objects.map((object) => {
      const plain = { ...object, checksums: { ...object.checksums } };
      if (plain.httpMetadata !== undefined) {
        plain.httpMetadata = withoutUnsetFields(plain.httpMetadata);
      }
      return plain;
    }),
  };
}

async function compareResponse(res, { head, body } = {}, bytes) {
  // Destructuring syntax looks ugly, but gets around needing to construct HeadResponse objects(somehow?)
  const { ...obj } = await res;
  obj.checksums = { ...obj.checksums };
  obj.httpMetadata = withoutUnsetFields(obj.httpMetadata);
  if (obj.range !== undefined) {
    obj.range = withoutUnsetFields(obj.range);
  }
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

// TODO: Let each test configure expected calls and responses shared by both transports.
// For example, head('missing') -> not found should produce HTTP 404 or JSRPC null.
// Each adapter should verify the method and arguments, reject unexpected calls, and encode
// the configured response in its transport's format.
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
        if (jsonRequest.object === 'must-not-call') {
          assert.fail('R2 backend must not be called for invalid input');
        }
        if (Object.hasOwn(stringUploadCases, jsonRequest.object)) {
          return handleStringUploadRequest(request, jsonRequest, metadataSize);
        }

        // The metadata has already been read from the body, so a Response cannot wrap it.
        const chunks = [];
        for await (const chunk of request.body) {
          chunks.push(chunk);
        }
        const uploadedBody = await new Blob(chunks).text();

        // Assert it's the correct version
        assert.strictEqual(jsonRequest.version, 1);

        if (jsonRequest.method === 'delete') {
          if (jsonRequest.object === 'boom') {
            return r2ErrorResponse({
              status: 400,
              v4code: 10021,
              message: 'bad keys',
            });
          }
          if (jsonRequest.objects) {
            // A batch delete can report individual key failures in a successful HTTP response.
            // R2Bucket::delete_() reads this body from the legacy transport but does not expose it.
            if (jsonRequest.objects[0] === perKeyErrorDeleteKeys[0]) {
              assert.deepEqual(jsonRequest.objects, perKeyErrorDeleteKeys);
              return Response.json(
                [
                  { name: 'existing' },
                  {
                    name: 'missing',
                    error: {
                      version: 1,
                      v4code: 10007,
                      message: 'The specified key does not exist.',
                    },
                  },
                ],
                { status: 200 }
              );
            }
            assert.deepEqual(jsonRequest.objects, [key, key + '2']);
          } else {
            assert.deepEqual(jsonRequest.object, key);
          }
          return new Response();
        }

        if (jsonRequest.object.startsWith('rpc-multipart-')) {
          return handleTrackedMultipartRequest(jsonRequest, uploadedBody);
        }

        switch (jsonRequest.object) {
          case 'basicKey': {
            if (jsonRequest.uploadId !== undefined) {
              assert(
                ['multipartId', 'resumedId'].includes(jsonRequest.uploadId)
              );
            }
            const resumed = jsonRequest.uploadId === 'resumedId';
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
                  etag: resumed ? 'resumedRpcPartEtag' : 'partEtag',
                });
              }
              case 'abortMultipartUpload': {
                return new Response();
              }
              case 'completeMultipartUpload': {
                return Response.json(
                  resumed
                    ? { ...objResponse, version: 'resumedRpcObjectVersion' }
                    : objResponse
                );
              }
            }
            break;
          }
          case 'rpcStream': {
            assert.strictEqual(uploadedBody, rpcStreamBody);
            if (jsonRequest.method === 'uploadPart') {
              assert.strictEqual(jsonRequest.uploadId, 'streamedId');
              return Response.json({ etag: 'partEtag' });
            }
            assert.strictEqual(jsonRequest.method, 'put');
            break;
          }
          case 'rpc-conditional-null': {
            assert.strictEqual(jsonRequest.method, 'put');
            assert.deepStrictEqual(jsonRequest.onlyIf, {
              etagMatches: [{ value: 'strongEtag', type: 'strong' }],
            });
            return r2ErrorResponse(preconditionFailedError);
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
          case 'onlyIfMultipleEtags': {
            assert.deepStrictEqual(jsonRequest.onlyIf, {
              etagMatches: [
                { value: 'strongEtag', type: 'strong' },
                { value: 'weakEtag', type: 'weak' },
              ],
              etagDoesNotMatch: [
                { value: 'firstEtag', type: 'strong' },
                { value: 'secondEtag', type: 'strong' },
              ],
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
                return Response.json(objResponse);
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
                return Response.json(objResponse);
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
          case 'blobBody': {
            assert.strictEqual(jsonRequest.method, 'put');
            assert.strictEqual(uploadedBody, body);
            break;
          }
        }
        return Response.json(objResponse);
      }
      case 'GET': {
        const rawHeader = request.headers.get('cf-r2-request');
        const jsonRequest = JSON.parse(rawHeader);
        assert.strictEqual(jsonRequest.version, 1);
        if (jsonRequest.object === 'must-not-call') {
          assert.fail('R2 backend must not be called for invalid input');
        }
        if (jsonRequest.method === 'list') {
          // Without r2_list_honor_include, the binding always asks for both kinds of metadata.
          // Each config tells the mock which behavior its R2 bindings use.
          const honorsIncludes = env.R2_LIST_HONOR_INCLUDE !== 'false';
          const { include = [], ...listRequest } = jsonRequest;
          const assertInclude = (requested) =>
            assert.deepStrictEqual(
              include,
              honorsIncludes ? requested : [0, 1]
            );
          const listResponse = (result) =>
            buildGetResponse({ head: result, isList: true });
          switch (jsonRequest.prefix) {
            case undefined: {
              assertInclude([]);
              assert.deepStrictEqual(listRequest, {
                method: 'list',
                newRuntime: true,
                version: 1,
              });
              return listResponse({ objects: [], truncated: false });
            }
            case 'basic': {
              assertInclude([0, 1]);
              assert.deepEqual(listRequest, {
                cursor: 'ai',
                delimiter: '/',
                limit: 1,
                method: 'list',
                newRuntime: true,
                prefix: 'basic',
                version: 1,
              });
              return listResponse({
                objects: [objResponse],
                truncated: true,
                cursor: 'ai',
              });
            }
            case 'httpMeta': {
              assert.deepEqual(listRequest, {
                method: 'list',
                newRuntime: true,
                prefix: 'httpMeta',
                version: 1,
              });
              // An empty include list returns objects without optional metadata.
              if (include.length === 0) {
                return listResponse({
                  objects: [objResponse],
                  truncated: false,
                });
              }
              assertInclude([0]);
              return listResponse({
                objects: [{ ...objResponse, httpFields, customFields: [] }],
                truncated: false,
              });
            }
            case 'customMeta': {
              assertInclude([1]);
              assert.deepEqual(listRequest, {
                method: 'list',
                newRuntime: true,
                prefix: 'customMeta',
                version: 1,
              });
              return listResponse({
                objects: [{ ...objResponse, httpFields: {}, customFields }],
                truncated: false,
              });
            }
            // Returns only the kinds of metadata that the request includes, which are empty.
            case 'rpc-metadata': {
              assert.deepStrictEqual(listRequest, {
                method: 'list',
                newRuntime: true,
                prefix: 'rpc-metadata',
                version: 1,
              });
              return listResponse({
                objects: [
                  {
                    ...objResponse,
                    ...(include.includes(0) && { httpFields: {} }),
                    ...(include.includes(1) && { customFields: [] }),
                  },
                ],
                truncated: false,
              });
            }
            case 'rpc-options': {
              assertInclude([]);
              assert.deepStrictEqual(listRequest, {
                cursor: 'cursor-in',
                delimiter: '/',
                limit: 2,
                method: 'list',
                newRuntime: true,
                prefix: 'rpc-options',
                startAfter: 'after',
                version: 1,
              });
              return listResponse({
                objects: [],
                truncated: true,
                cursor: 'cursor-out',
                delimitedPrefixes: ['rpc-options/'],
              });
            }
            case 'rpc-boom':
              return r2ErrorResponse(noSuchBucketError);
          }
          throw new Error(`Unexpected list prefix: ${jsonRequest.prefix}`);
        }
        assert(['get', 'head'].includes(jsonRequest.method));
        switch (jsonRequest.object) {
          case 'basicKey':
          case 'rpc-lazy-body':
          case 'rpc-bytes':
          case 'rpc-array-buffer':
          case 'rpc-blob': {
            return buildGetResponse({ body });
          }
          case 'missing':
            return r2ErrorResponse(noSuchKeyError);
          case 'boom':
          case 'rpc-get-boom':
            return r2ErrorResponse(noSuchBucketError);
          // The binding converts a non-string key to a string.
          case '12345': {
            assert.strictEqual(jsonRequest.method, 'head');
            return buildGetResponse();
          }
          case 'ranged': {
            assert.strictEqual(jsonRequest.method, 'head');
            return buildGetResponse({
              head: { range: { offset: '10', length: '20' } },
            });
          }
          case 'rpc-json': {
            assert.strictEqual(jsonRequest.method, 'get');
            const jsonBody = JSON.stringify({ ok: true });
            return buildGetResponse({
              head: { size: String(jsonBody.length) },
              body: jsonBody,
            });
          }
          case 'rpc-conditional-metadata': {
            assert.strictEqual(jsonRequest.method, 'get');
            if (jsonRequest.onlyIf !== undefined) {
              assert.deepStrictEqual(jsonRequest.onlyIf, {
                etagMatches: [{ value: 'objectEtag', type: 'strong' }],
              });
            }
            return buildGetResponse({ error: preconditionFailedError });
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
                  offset: '5',
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
          case 'onlyIfMultipleEtags': {
            assert.deepStrictEqual(jsonRequest.onlyIf, {
              etagMatches: [
                { value: 'strongEtag', type: 'strong' },
                { value: 'weakEtag', type: 'weak' },
              ],
              etagDoesNotMatch: [
                { value: 'firstEtag', type: 'strong' },
                { value: 'secondEtag', type: 'strong' },
              ],
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
      // PutObject with a Blob body
      await compareResponse(env.BUCKET.put('blobBody', new Blob([body])));
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
        assert.deepEqual(plainListResult(list), {
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
              offset: 5,
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
      const multipleEtagHeaders = new Headers({
        'if-match': '"strongEtag", W/"weakEtag"',
        'if-none-match': '"firstEtag", "secondEtag"',
      });
      await env.BUCKET.put('onlyIfMultipleEtags', body, {
        onlyIf: multipleEtagHeaders,
      });
      await env.BUCKET.get('onlyIfMultipleEtags', {
        onlyIf: multipleEtagHeaders,
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
          assert.deepEqual(plainListResult(list), {
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
          ).complete([])
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
          assert.deepEqual(plainListResult(list), {
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
          ).complete([])
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
      assert.deepStrictEqual(withoutUnsetFields(headResp.checksums.toJSON()), {
        md5: '9a0364b9e99bb480dd25e1f0284c8555',
        sha1: '2a0364b9e99bb480dd25e1f0284c855511223344',
        sha256:
          '3a0364b9e99bb480dd25e1f0284c8555112233445566778899aabbccddeeff00',
      });
    }
  },
};

// Test that documents the behaviour of multi-key deletion where one fails and the other succeeds
export const deletePerKeyErrorParityTests = {
  async test(_ctrl, env) {
    if (env.R2_TRANSPORT !== 'http' && env.R2_TRANSPORT !== 'jsrpc') {
      return;
    }

    // Both transports implement Promise<void>: the legacy path discards the successful HTTP body,
    // while the RPC path receives the corresponding void result from its backend entrypoint.
    const deletion = env.BUCKET.delete(perKeyErrorDeleteKeys);
    assert(deletion instanceof Promise);
    assert.strictEqual(await deletion, undefined);
  },
};

export const r2ValidationTests = {
  async test(ctrl, env) {
    await assert.rejects(
      env.BUCKET.get('must-not-call', { range: { offset: -1 } }),
      {
        message:
          'Invalid range. Starting offset (-1) must be greater than or equal to 0.',
      }
    );
    await assert.rejects(
      env.BUCKET.get('must-not-call', { range: { suffix: 1, offset: 0 } }),
      { name: 'TypeError', message: 'Suffix is incompatible with offset.' }
    );
    await assert.rejects(
      env.BUCKET.get('must-not-call', { range: { suffix: 1, length: 1 } }),
      { name: 'TypeError', message: 'Suffix is incompatible with length.' }
    );
    for (const field of ['offset', 'length', 'suffix']) {
      for (const value of [-1, 0.5, NaN]) {
        await assert.rejects(
          env.BUCKET.get('must-not-call', { range: { [field]: value } }),
          { name: 'RangeError' }
        );
      }
    }
    await assert.rejects(env.BUCKET.get('must-not-call', { ssecKey: 'bad' }), {
      message: 'SSE-C Key must be 32 bytes in length',
    });
    await assert.rejects(
      env.BUCKET.get('must-not-call', {
        onlyIf: { etagMatches: '"quoted"' },
      }),
      {
        message: 'Conditional ETag should not be wrapped in quotes ("quoted").',
      }
    );

    await assert.rejects(
      env.BUCKET.put('must-not-call', body, { md5: new Uint8Array(1) }),
      { message: 'MD5 is 16 bytes, not 1' }
    );
    await assert.rejects(
      env.BUCKET.put('must-not-call', body, {
        md5: 'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz',
      }),
      { message: "Provided MD5 wasn't a valid hex string" }
    );
    await assert.rejects(
      env.BUCKET.put('must-not-call', body, {
        md5: md5Buffer,
        sha1: sha1Buffer,
      }),
      { message: 'You cannot specify multiple hashing algorithms.' }
    );

    const invalidOptionsStream = new FixedLengthStream(1);
    const invalidOptionsWriter = invalidOptionsStream.writable.getWriter();
    const invalidOptionsWriting = invalidOptionsWriter.write(
      new Uint8Array([1])
    );
    await assert.rejects(
      env.BUCKET.put('must-not-call', invalidOptionsStream.readable, {
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

    await assert.rejects(
      env.BUCKET.createMultipartUpload('must-not-call', {
        ssecKey: 'bad',
      }),
      { message: 'SSE-C Key must be 32 bytes in length' }
    );
    const invalidPartUpload = env.BUCKET.resumeMultipartUpload(
      'must-not-call',
      'invalidPartUploadId'
    );
    await assert.rejects(
      invalidPartUpload.uploadPart(1, body, { ssecKey: 'bad' }),
      { message: 'SSE-C Key must be 32 bytes in length' }
    );
    await assert.rejects(
      invalidPartUpload.complete([{ partNumber: 0, etag: 'invalid' }]),
      {
        message:
          'Part number must be between 1 and 10000 (inclusive). Actual value was: 0',
      }
    );
  },
};

// Public R2 binding behavior that both transports implement the same way. The JSRPC configs run
// these through ObservedR2Binding, so results and errors come from the same HTTP mock as the HTTP
// config.
export const r2BindingApiTests = {
  async test(ctrl, env) {
    assert.strictEqual(await env.BUCKET.head('missing'), null);
    assert.strictEqual(await env.BUCKET.get('missing'), null);

    const ranged = await env.BUCKET.head('ranged');
    assert.deepStrictEqual(withoutUnsetFields(ranged.range), {
      offset: 10,
      length: 20,
    });

    const coerced = await env.BUCKET.head(12345);
    assert.strictEqual(coerced.key, key);

    // Backend errors surface as plain Errors, without an R2Error code property.
    await assert.rejects(env.BUCKET.head('boom'), (err) => {
      assert.strictEqual(err.message, 'head: no such bucket (10006)');
      assert.strictEqual(err.code, undefined);
      return true;
    });
    await assert.rejects(env.BUCKET.get('rpc-get-boom'), {
      message: 'get: no such bucket (10006)',
    });
    await assert.rejects(env.BUCKET.delete('boom'), {
      message: 'delete: bad keys (10021)',
    });

    const conditional = await env.BUCKET.get('rpc-conditional-metadata', {
      onlyIf: { etagMatches: 'objectEtag' },
    });
    assert.strictEqual(conditional.body, undefined);
    assert.deepStrictEqual(withoutUnsetFields(conditional.httpMetadata), {});
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

    const emptyList = await env.BUCKET.list();
    assert.deepStrictEqual(plainListResult(emptyList), {
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
    assert.deepStrictEqual(plainListResult(optionList), {
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
          assert.deepStrictEqual(withoutUnsetFields(object.httpMetadata), {});
        }
        if (include.includes('customMetadata')) {
          assert.deepStrictEqual(object.customMetadata, {});
        }
      }
    }

    await assert.rejects(env.BUCKET.list({ prefix: 'rpc-boom' }), {
      message: 'list: no such bucket (10006)',
    });

    assert.strictEqual(
      await env.BUCKET.put('rpc-conditional-null', body, {
        onlyIf: { etagMatches: 'strongEtag' },
      }),
      null
    );

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
    // The direct HTTP path leaves a validation-failed stream with its caller. Releasing an
    // RPC-transferred stream is handled by the RPC transport.
    if (env.R2_TRANSPORT !== 'jsrpc') {
      assert.strictEqual(unknownLengthCancelReason, undefined);
    }
  },
};

// Multipart behavior that both transports implement the same way. See r2BindingApiTests.
export const r2MultipartApiTests = {
  async test(ctrl, env) {
    const resumed = env.BUCKET.resumeMultipartUpload(key, 'resumedId');
    assert.strictEqual(resumed.key, key);
    assert.strictEqual(resumed.uploadId, 'resumedId');
    const resumedPart = await resumed.uploadPart(1, body);
    assert.deepStrictEqual(resumedPart, {
      partNumber: 1,
      etag: 'resumedRpcPartEtag',
    });
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
    assert.strictEqual(JSON.stringify(resumedObject.checksums), '{}');

    const encodedStreamBody = new TextEncoder().encode(rpcStreamBody);
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

    // Each operation carries its key and upload ID, including interleaved calls on the same key.
    const uploads = [];
    for (const uploadKey of ['rpc-multipart-first', 'rpc-multipart-second']) {
      const headers = new Headers(httpMetaHeaders);
      const metadata = { uploadKey };
      const creating = env.BUCKET.createMultipartUpload(uploadKey, {
        httpMetadata: headers,
        customMetadata: metadata,
      });
      headers.set('content-type', 'application/octet-stream');
      metadata.uploadKey = 'mutated';
      const upload = await creating;
      assert.strictEqual(upload.key, uploadKey);
      assert.strictEqual(upload.uploadId, `${uploadKey}-id`);
      uploads.push(upload);
    }
    // More uploads for the same key, addressed through wrappers resumed from their IDs.
    for (const uploadId of [
      'rpc-multipart-first-id-2',
      'rpc-multipart-first-id-3',
    ]) {
      const created = await env.BUCKET.createMultipartUpload(
        'rpc-multipart-first'
      );
      assert.strictEqual(created.uploadId, uploadId);
      const upload = env.BUCKET.resumeMultipartUpload(
        'rpc-multipart-first',
        uploadId
      );
      assert.strictEqual(upload.key, 'rpc-multipart-first');
      assert.strictEqual(upload.uploadId, uploadId);
      uploads.push(upload);
    }
    const uploadedParts = await Promise.all(
      uploads.map(async (upload) => {
        const first = await upload.uploadPart(1, body);
        const rest = await Promise.all([
          upload.uploadPart(2, body),
          upload.uploadPart(3, body),
        ]);
        const parts = [first, ...rest];
        assert.deepStrictEqual(
          parts,
          [1, 2, 3].map((partNumber) => ({
            partNumber,
            etag: `${upload.key}/${upload.uploadId}/${partNumber}`,
          }))
        );
        return parts;
      })
    );

    for (let i = 0; i < uploads.length; i++) {
      const upload = uploads[i];
      const result = await upload.complete(uploadedParts[i]);
      assert.strictEqual(result.key, upload.key);
      assert.strictEqual(result.version, upload.uploadId);
      // Created and resumed uploads both use the metadata returned by the backend.
      assert.deepStrictEqual(result.customMetadata, {});
      const resultHeaders = new Headers();
      result.writeHttpMetadata(resultHeaders);
      assert.deepStrictEqual([...resultHeaders], []);
    }

    const aborted = await env.BUCKET.createMultipartUpload(
      'rpc-multipart-aborted'
    );
    await aborted.uploadPart(1, body);
    // Abort must address the same upload when called through a separately resumed wrapper.
    await env.BUCKET.resumeMultipartUpload(
      aborted.key,
      aborted.uploadId
    ).abort();
    const missing = env.BUCKET.resumeMultipartUpload(
      'rpc-multipart-missing',
      'missing'
    );
    for (const upload of [aborted, missing]) {
      await assert.rejects(upload.uploadPart(2, body), {
        message: 'uploadPart: Upload not found (10024)',
      });
      await assert.rejects(upload.complete([]), {
        message: 'completeMultipartUpload: Upload not found (10024)',
      });
      await upload.abort();
      await upload.abort();
    }
  },
};

function stringUploadValue({ size, suffix }) {
  return (
    'a'.repeat(size - new TextEncoder().encode(suffix).byteLength) + suffix
  );
}

export const r2StringPutTests = {
  async test(_ctrl, env) {
    if (env.R2_STRING_UPLOAD_TEST !== 'true') return;
    for (const [key, fixture] of Object.entries(stringUploadCases)) {
      const result = await env.BUCKET.put(key, stringUploadValue(fixture));
      assert.strictEqual(result.key, key);
      assert.strictEqual(result.size, fixture.size);
    }
  },
};

export const r2StringUploadPartTests = {
  async test(_ctrl, env) {
    if (env.R2_STRING_UPLOAD_TEST !== 'true') return;
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

export default testWorker;
