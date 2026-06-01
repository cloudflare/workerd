// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-21:
// R2 writeHttpMetadata must reject metadata values containing invalid header
// bytes (NUL, CR, LF) instead of silently inserting them into the Headers
// object via the unvalidated Headers::setCommon path.

import assert from 'node:assert';

const objResponse = {
  name: 'test-key',
  version: 'objectVersion',
  size: '7',
  etag: 'objectEtag',
  uploaded: '1724767257918',
  storageClass: 'Standard',
};

// The httpFields that the mock R2 backend returns — includes a CRLF injection
// in contentType. This simulates an attacker who stored malicious metadata.
const maliciousHttpFields = {
  contentType: 'text/plain\r\nX-Injected: yes',
};

const nulHttpFields = {
  contentDisposition: 'attachment; filename="evil\x00.txt"',
};

const validHttpFields = {
  contentType: 'text/html',
  cacheControl: 'no-store',
};

function buildGetResponse(httpFields) {
  const encoder = new TextEncoder();
  const meta = {
    ...objResponse,
    httpFields,
  };
  const metadata = encoder.encode(JSON.stringify(meta));
  const body = encoder.encode('payload');
  const responseBody = new ReadableStream({
    start(controller) {
      controller.enqueue(metadata);
      controller.enqueue(body);
      controller.close();
    },
  });
  return new Response(responseBody, {
    headers: {
      'cf-r2-metadata-size': metadata.length.toString(),
      'content-length': (metadata.length + body.length).toString(),
    },
  });
}

// Track which httpFields were stored per object name
const storedHttpFields = {};

export default {
  // Mock R2 backend: handles the HTTP requests that the R2 bucket binding makes
  async fetch(request) {
    assert(['GET', 'PUT'].includes(request.method));

    if (request.method === 'PUT') {
      const metadataSizeString = request.headers.get('cf-r2-metadata-size');
      assert.notStrictEqual(metadataSizeString, null);

      const metadataSize = parseInt(metadataSizeString);
      assert(!Number.isNaN(metadataSize));

      const reader = request.body.getReader({ mode: 'byob' });
      const jsonArray = new Uint8Array(metadataSize);
      const { value } = await reader.readAtLeast(metadataSize, jsonArray);
      reader.releaseLock();

      const jsonRequest = JSON.parse(new TextDecoder().decode(value));

      // Consume remaining body
      for await (const _ of request.body) {
        // intentionally empty
      }

      // Store the httpFields for later retrieval
      storedHttpFields[jsonRequest.object] = jsonRequest.httpFields || {};

      return Response.json({
        ...objResponse,
        name: jsonRequest.object,
        httpFields: jsonRequest.httpFields,
      });
    }

    if (request.method === 'GET') {
      // GET requests carry the R2 request metadata in a header, not the body
      const rawHeader = request.headers.get('cf-r2-request');
      const jsonRequest = JSON.parse(rawHeader);

      // Return the stored httpFields for the requested object
      const httpFields = storedHttpFields[jsonRequest.object] || {};

      return buildGetResponse(httpFields);
    }

    return new Response('Not found', { status: 404 });
  },
};

export const writeHttpMetadataValidation = {
  async test(ctrl, env) {
    // 1. Store an R2 object with a contentType containing CRLF (header injection payload).
    await env.BUCKET.put('crlf-test', 'payload', {
      httpMetadata: maliciousHttpFields,
    });

    const obj = await env.BUCKET.get('crlf-test');
    assert.ok(obj !== null, 'R2 object should exist');

    // After the fix, writeHttpMetadata must throw a TypeError because the
    // stored contentType value contains \r\n which fails header value validation.
    const headers = new Headers();
    assert.throws(
      () => obj.writeHttpMetadata(headers),
      (err) => {
        assert.ok(
          err instanceof TypeError,
          `Expected TypeError, got ${err.constructor.name}`
        );
        return true;
      },
      'writeHttpMetadata should throw TypeError for CRLF in metadata value'
    );

    // 2. Also test NUL byte in contentDisposition
    await env.BUCKET.put('nul-test', 'payload', {
      httpMetadata: nulHttpFields,
    });

    const obj2 = await env.BUCKET.get('nul-test');
    assert.ok(obj2 !== null, 'R2 object should exist');

    const headers2 = new Headers();
    assert.throws(
      () => obj2.writeHttpMetadata(headers2),
      (err) => {
        assert.ok(
          err instanceof TypeError,
          `Expected TypeError, got ${err.constructor.name}`
        );
        return true;
      },
      'writeHttpMetadata should throw TypeError for NUL in metadata value'
    );

    // 3. Verify that valid metadata still works correctly
    await env.BUCKET.put('valid-test', 'payload', {
      httpMetadata: validHttpFields,
    });

    const obj3 = await env.BUCKET.get('valid-test');
    assert.ok(obj3 !== null, 'R2 object should exist');

    const headers3 = new Headers();
    obj3.writeHttpMetadata(headers3);
    assert.strictEqual(headers3.get('content-type'), 'text/html');
    assert.strictEqual(headers3.get('cache-control'), 'no-store');
  },
};
