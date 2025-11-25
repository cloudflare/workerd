// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';
export default class KVTest extends WorkerEntrypoint {
  // Request handler (from `env.NAMESPACE`)
  async fetch(request, env, ctx) {
    let result = 'example';
    const { pathname, searchParams } = new URL(request.url);
    const queryParameters = Object.fromEntries(searchParams);
    const headers = new Headers();
    headers.set('CF-Cache-Status', 'HIT');

    if (pathname === '/fail-client') {
      return new Response(null, { status: 404 });
    } else if (pathname == '/fail-server') {
      return new Response(null, { status: 500 });
    } else if (pathname == '/get-json') {
      result = JSON.stringify({ example: 'values' });
    } else if (pathname == '/bulk/get') {
      let r = '';
      const decoder = new TextDecoder();
      for await (const chunk of request.body) {
        r += decoder.decode(chunk, { stream: true });
      }
      r += decoder.decode();
      const parsedBody = JSON.parse(r);
      const keys = parsedBody.keys;
      if (keys.length > 100) {
        return new Response(null, {
          status: 400,
          statusText: 'You can request a maximum of 100 keys',
        });
      }
      if (keys.length < 1) {
        return new Response(null, {
          status: 400,
          statusText: 'You must request a minimum of 1 key',
        });
      }
      result = {};
      if (parsedBody.type == 'json') {
        for (const key of keys) {
          if (key == 'key-not-json') {
            return new Response(
              'At least one of the requested keys corresponds to a non-json value',
              {
                status: 400,
                statusText:
                  'At least one of the requested keys corresponds to a non-json value',
              }
            );
          }
          const val = { example: `values-${key}` };
          if (parsedBody.withMetadata) {
            result[key] = { value: val, metadata: 'example-metadata' };
          } else {
            result[key] = val;
          }
        }
      } else if (!parsedBody.type || parsedBody.type == 'text') {
        for (const key of keys) {
          const val = JSON.stringify({ example: `values-${key}` });
          if (key == 'not-found') {
            result[key] = null;
          } else if (parsedBody.withMetadata) {
            result[key] = { value: val, metadata: 'example-metadata' };
          } else {
            result[key] = val;
          }
        }
      } else {
        // invalid type requested
        return new Response(
          `"${parsedBody.type}" is not a valid type. Use "json" or "text"`,
          {
            status: 400,
            statusText: `"${parsedBody.type}" is not a valid type. Use "json" or "text"`,
          }
        );
      }
      result = JSON.stringify(result);
    } else if (
      pathname === '/foo_with_exp' ||
      pathname === '/foo_with_expTtl'
    ) {
      console.log(pathname, searchParams, request.method, await request.text());
      result = JSON.stringify({
        keys: [],
      });
    } else if (pathname === '/') {
      const limit = parseInt(queryParameters.key_count_limit ?? '3', 10);
      const prefix = queryParameters.prefix;

      switch (prefix) {
        case 'te':
          // [0, 1, 2, ... limit]
          const range = [...Array(limit)].keys();
          const keys = Array.from(range).map((index) => ({
            name: `test${index}`,
            metadata: { someMetadataKey: 'someMetadataValue' },
          }));

          result = JSON.stringify({
            keys,
            list_complete: limit === 100,
            cursor: limit === 100 ? undefined : '6Ck1la0VxJ0djhidm1MdX2FyD',
            expiration: 1234,
          });
          break;
        case 'not-found':
          result = JSON.stringify({
            keys: [],
            list_complete: true,
          });
      }
    } else {
      // generic success for get key
      result = 'value-' + pathname.slice(1);
    }
    let response = new Response(result, {
      status: 200,
      headers: headers,
    });
    response.headers.set(
      'CF-KV-Metadata',
      '{"someMetadataKey":"someMetadataValue","someUnicodeMeta":"🤓"}'
    );
    return response;
  }

  async delete(keys) {
    if (keys === 'error') {
      // invalid type requested
      throw new KVInternalError('failed to delete a single key', 'DELETE');
    }
    if (Array.isArray(keys) && keys.length > 100) {
      throw new BadClientRequestError(
        'You can delete maximum of 100 keys per request',
        'DELETE'
      );
    }
    if (Array.isArray(keys) && keys.includes('error')) {
      throw new KVInternalError(
        'failed to delete a single key from batch',
        'DELETE'
      );
    }
    // Success otherwise
  }
}

class BadClientRequestError extends Error {
  constructor(message, operation) {
    super(`KV ${operation} failed: 400 ${message}`);
  }
}

class KVInternalError extends Error {
  constructor(message, operation) {
    super(`KV ${operation} failed: 500 ${message}`);
  }
}

export let getTest = {
  async test(ctrl, env, ctx) {
    // Test .get()
    let response = await env.KV.get('success', {});
    assert.strictEqual(response, 'value-success');

    response = await env.KV.get('fail-client');
    assert.strictEqual(response, null);
    await assert.rejects(env.KV.get('fail-server'), {
      message: 'KV GET failed: 500 Internal Server Error',
    });

    response = await env.KV.get('get-json');
    assert.strictEqual(response, JSON.stringify({ example: 'values' }));

    response = await env.KV.get('get-json', 'json');
    assert.deepStrictEqual(response, { example: 'values' });

    response = await env.KV.get('success', 'stream');
    let result = '';
    const decoder = new TextDecoder();
    for await (const chunk of response) {
      result += decoder.decode(chunk, { stream: true });
    }
    result += decoder.decode();
    assert.strictEqual(result, 'value-success');

    response = await env.KV.get('success', 'arrayBuffer');
    assert.strictEqual(new TextDecoder().decode(response), 'value-success');
  },
};

export let getBulkTest = {
  async test(ctrl, env, ctx) {
    // // Testing .get bulk
    let response = await env.KV.get(['key1', 'key"2']);
    let expected = new Map([
      ['key1', '{"example":"values-key1"}'],
      ['key"2', '{"example":"values-key\\"2"}'],
    ]);
    assert.deepStrictEqual(response, expected);

    response = await env.KV.get(['key1', 'key2'], {});
    expected = new Map([
      ['key1', '{"example":"values-key1"}'],
      ['key2', '{"example":"values-key2"}'],
    ]);
    assert.deepStrictEqual(response, expected);

    let fullKeysArray = [];
    let fullResponse = new Map();
    for (let i = 0; i < 100; i++) {
      fullKeysArray.push(`key` + i);
      fullResponse.set(`key` + i, `{"example":"values-key${i}"}`);
    }

    response = await env.KV.get(fullKeysArray, {});
    assert.deepStrictEqual(response, fullResponse);

    //sending over 100 keys
    fullKeysArray.push('key100');
    await assert.rejects(env.KV.get(fullKeysArray), {
      message: 'KV GET_BULK failed: 400 You can request a maximum of 100 keys',
    });

    response = await env.KV.get(['key1', 'not-found'], { cacheTtl: 100 });
    expected = new Map([
      ['key1', '{"example":"values-key1"}'],
      ['not-found', null],
    ]);
    assert.deepStrictEqual(response, expected);

    await assert.rejects(env.KV.get([]), {
      message: 'KV GET_BULK failed: 400 You must request a minimum of 1 key',
    });

    // // get bulk json
    response = await env.KV.get(['key1', 'key2'], 'json');
    expected = new Map([
      ['key1', { example: 'values-key1' }],
      ['key2', { example: 'values-key2' }],
    ]);
    assert.deepStrictEqual(response, expected);

    // // get bulk json but it is not json - throws error
    await assert.rejects(env.KV.get(['key-not-json', 'key2'], 'json'), {
      message:
        'KV GET_BULK failed: 400 At least one of the requested keys corresponds to a non-json value',
    });

    // // requested type is invalid for bulk get
    await assert.rejects(env.KV.get(['key-not-json', 'key2'], 'arrayBuffer'), {
      message:
        'KV GET_BULK failed: 400 "arrayBuffer" is not a valid type. Use "json" or "text"',
    });

    await assert.rejects(
      env.KV.get(['key-not-json', 'key2'], { type: 'banana' }),
      {
        message:
          'KV GET_BULK failed: 500 "banana" is not a valid type. Use "json" or "text"',
      }
    );

    // // get with metadata
    response = await env.KV.getWithMetadata('key1');
    expected = {
      value: 'value-key1',
      metadata: { someMetadataKey: 'someMetadataValue', someUnicodeMeta: '🤓' },
      cacheStatus: 'HIT',
    };
    assert.deepStrictEqual(response, expected);

    response = await env.KV.getWithMetadata(['key1']);
    expected = new Map([
      [
        'key1',
        { metadata: 'example-metadata', value: '{"example":"values-key1"}' },
      ],
    ]);
    assert.deepStrictEqual(response, expected);

    response = await env.KV.getWithMetadata(['key1'], 'json');
    expected = new Map([
      [
        'key1',
        { metadata: 'example-metadata', value: { example: 'values-key1' } },
      ],
    ]);
    assert.deepStrictEqual(response, expected);
    response = await env.KV.getWithMetadata(['key1', 'key2'], 'json');
    expected = new Map([
      [
        'key1',
        { metadata: 'example-metadata', value: { example: 'values-key1' } },
      ],
      [
        'key2',
        { metadata: 'example-metadata', value: { example: 'values-key2' } },
      ],
    ]);
    assert.deepStrictEqual(response, expected);
  },
};

export let deleteBulkTest = {
  async test(ctrl, env, ctx) {
    // Single key
    let result = await env.KV.deleteBulk('success');
    assert.strictEqual(result, undefined);

    // Failure
    await assert.rejects(env.KV.deleteBulk('error'), {
      message: 'KV DELETE failed: 500 failed to delete a single key',
    });

    // Multiple keys
    result = await env.KV.deleteBulk(['key1', 'key2', 'key3']);
    assert.strictEqual(result, undefined);

    // Too many keys
    await assert.rejects(
      env.KV.deleteBulk(
        Array.from({ length: 101 }, (_, index) => `key${index + 1}`)
      ),
      {
        message:
          'KV DELETE failed: 400 You can delete maximum of 100 keys per request',
      }
    );

    // Multiple keys with failure
    await assert.rejects(env.KV.deleteBulk(['key1', 'error']), {
      message: 'KV DELETE failed: 500 failed to delete a single key from batch',
    });
  },
};

export let listTest = {
  async test(ctrl, env, ctx) {
    let result = await env.KV.list({ prefix: 'te' });
    assert.strictEqual(result.keys.length, 3);
    assert.strictEqual(result.list_complete, false);
    assert.strictEqual(result.cursor, '6Ck1la0VxJ0djhidm1MdX2FyD');

    result = await env.KV.list({ prefix: 'te', limit: 100, cursor: '123' });
    assert.strictEqual(result.keys.length, 100);
    assert.strictEqual(result.list_complete, true);

    result = await env.KV.list({ prefix: 'not-found' });
    assert.deepEqual(result, {
      keys: [],
      list_complete: true,
      cacheStatus: 'HIT',
    });
  },
};

export let putTest = {
  async test(ctrl, env, ctx) {
    await env.KV.put('foo_with_exp', 'bar1', { expiration: 10 });
    await env.KV.put('foo_with_expTtl', 'bar2 a bit longer value', {
      expirationTtl: 15,
    });
    const buf = new ArrayBuffer(256);
    await env.KV.put('foo_with_expTtl', buf, { expirationTtl: 15 });
    // assert.strictEqual(result.keys.length, 3);
    // assert.strictEqual(result.list_complete, false);
    // assert.strictEqual(result.cursor, '6Ck1la0VxJ0djhidm1MdX2FyD');

    // result = await env.KV.list({ prefix: 'te', limit: 100, cursor: '123' });
    // assert.strictEqual(result.keys.length, 100);
    // assert.strictEqual(result.list_complete, true);

    // result = await env.KV.list({ prefix: 'not-found' });
    // assert.deepEqual(result, {
    //   keys: [],
    //   list_complete: true,
    //   cacheStatus: null,
    // });
  },
};
