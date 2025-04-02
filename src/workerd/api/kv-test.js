// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
export default {
  // Producer receiver (from `env.NAMESPACE`)
  async fetch(request, env, ctx) {
    let result = 'example';
    const { pathname } = new URL(request.url);
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
      if (keys.length < 1 || keys.length > 100) {
        return new Response(null, { status: 400 });
      }
      result = {};
      if (parsedBody.type == 'json') {
        for (const key of keys) {
          if (key == 'key-not-json') {
            return new Response(null, { status: 500 });
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
        return new Response(null, { status: 500 });
      }
      result = JSON.stringify(result);
    } else {
      // generic success for get key
      result = 'value-' + pathname.slice(1);
    }

    let response = new Response(result, { status: 200 });
    response.headers.set(
      'CF-KV-Metadata',
      '{"someMetadataKey":"someMetadataValue","someUnicodeMeta":"🤓"}'
    );

    return response;
  },

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

    // // Testing .get bulk
    response = await env.KV.get(['key1', 'key"2']);
    let expected = new Map([
      ['key1', '{\"example\":\"values-key1\"}'],
      ['key"2', '{\"example\":\"values-key\\"2\"}'],
    ]);
    assert.deepStrictEqual(response, expected);

    response = await env.KV.get(['key1', 'key2'], {});
    expected = new Map([
      ['key1', '{\"example\":\"values-key1\"}'],
      ['key2', '{\"example\":\"values-key2\"}'],
    ]);
    assert.deepStrictEqual(response, expected);

    let fullKeysArray = [];
    let fullResponse = new Map();
    for (let i = 0; i < 100; i++) {
      fullKeysArray.push(`key` + i);
      fullResponse.set(`key` + i, `{\"example\":\"values-key${i}\"}`);
    }

    response = await env.KV.get(fullKeysArray, {});
    assert.deepStrictEqual(response, fullResponse);

    //sending over 100 keys
    fullKeysArray.push('key100');
    await assert.rejects(env.KV.get(fullKeysArray), {
      message: 'KV GET_BULK failed: 400 Bad Request',
    });

    response = await env.KV.get(['key1', 'not-found'], { cacheTtl: 100 });
    expected = new Map([
      ['key1', '{\"example\":\"values-key1\"}'],
      ['not-found', null],
    ]);
    assert.deepStrictEqual(response, expected);

    await assert.rejects(env.KV.get([]), {
      message: 'KV GET_BULK failed: 400 Bad Request',
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
      message: 'KV GET_BULK failed: 500 Internal Server Error',
    });

    // // requested type is invalid for bulk get
    await assert.rejects(env.KV.get(['key-not-json', 'key2'], 'arrayBuffer'), {
      message: 'KV GET_BULK failed: 500 Internal Server Error',
    });

    await assert.rejects(
      env.KV.get(['key-not-json', 'key2'], { type: 'banana' }),
      {
        message: 'KV GET_BULK failed: 500 Internal Server Error',
      }
    );

    // // get with metadata
    response = await env.KV.getWithMetadata('key1');
    expected = {
      value: 'value-key1',
      metadata: { someMetadataKey: 'someMetadataValue', someUnicodeMeta: '🤓' },
      cacheStatus: null,
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
