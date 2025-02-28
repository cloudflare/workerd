// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
export default {
  // Producer receiver (from `env.NAMESPACE`)
  async fetch(request, env, ctx) {
    const options = {
      status: 200,
      statusText: "Success!",
      headers: new Headers({
        'Content-Type': 'application/json'
      })
    };

    var result = "example";
    const { pathname } = new URL(request.url);
    if (pathname === '/fail-client') {
      options.status = "404"
      result = ""
    } else if (pathname == "/fail-server") {
      options.status = "500"
      result = ""
    } else if (pathname == "/get-json") {
      result = JSON.stringify({ example: "values" });
    } else if (pathname == "/get-bulk") {
      var body = request.body;
      const reader = body.getReader();
      const decoder = new TextDecoder(); // UTF-8 by default
      let r = "";
      while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          r += decoder.decode(value, { stream: true });
      }
      const parsedBody = JSON.parse(r);
      const keys = parsedBody.keys;
      result = {}
      if(parsedBody.type == "json") {
        for(const key of keys) {
          if(key == "key-not-json") {
            return new Response(null, {status: 500})
          }
          const val = { example: "values"};
          if (parsedBody.withMetadata) {
            result[key] = {value: val, metadata: "example-metadata"};
          } else {
            result[key] = val;
          }
        }
      } else if (!parsedBody.type || parsedBody.type == "text") {
        for(const key of keys) {
          const val = JSON.stringify({ example: "values" });;
          if (parsedBody.withMetadata) {
            result[key] = {value: val, metadata: "example-metadata"};
          } else {
            result[key] = val;
          }
        }
      } else { // invalid type requested
        return new Response("Requested type is invalid",{status: 500});

      }
      result = JSON.stringify(result);
    } else { // generic success for get key
      result = "value-"+pathname.slice(1);
    }
    let response =  new Response(result, {status: 200});
    response.headers.set("CF-KV-Metadata", '{"someMetadataKey":"someMetadataValue","someUnicodeMeta":"🤓"}');

    return response;
  },


  async test(ctrl, env, ctx) {
    // Test .get()
    var response = await env.KV.get('success',{});
    assert.strictEqual(response, "value-success");

    response = await env.KV.get('fail-client');
    assert.strictEqual(response, "");
    try {
      response = await env.KV.get('fail-server');
      assert.ok(false);
    } catch {
      assert.ok(true);
    }

    response = await env.KV.get('get-json');
    assert.strictEqual(response, JSON.stringify({ example: "values" }));

    response = await env.KV.get('get-json', "json");
    assert.deepEqual(response, { example: "values" });


    var response = await env.KV.get('success', "stream");
    const reader = response.getReader();
    const decoder = new TextDecoder(); // UTF-8 by default
    let result = "";
    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        result += decoder.decode(value, { stream: true });
    }
    assert.strictEqual(result, "value-success");

    var response = await env.KV.get('success', "arrayBuffer");
    assert.strictEqual(new TextDecoder().decode(response), "value-success");


    // Testing .get bulk
    var response = await env.KV.get(["key1", "key2"],{});
    var expected = { key1: '{\"example\":\"values\"}', key2: '{\"example\":\"values\"}' };
    assert.deepEqual(response, expected);


    // get bulk text but it is json format
    var response = await env.KV.get(["key1", "key2"], "json");
    var expected = { key1: { example: 'values' }, key2: { example: 'values' } };
    assert.deepEqual(response, expected);

    // get bulk json but it is not json - throws error
    try{
      var response = await env.KV.get(["key-not-json", "key2"], "json");
      assert.ok(false); // not reached
    } catch ({ name, message }){
      assert(message.includes("500"))
      assert.ok(true);
    }
    // requested type is invalid for bulk get
    try{
      var response = await env.KV.get(["key-not-json", "key2"], "arrayBuffer");
      assert.ok(false); // not reached
    } catch ({ name, message }){
      // assert(message.includes("invalid")) // this message is not processed, should it?
      assert.ok(true);
    }
    try{
      var response = await env.KV.get(["key-not-json", "key2"], {type: "banana"});
      assert.ok(false); // not reached
    } catch ({ name, message }){
      // assert(message.includes("invalid")) // this message is not processed, should it?
      assert.ok(true);
    }

    // get with metadata
    var response = await env.KV.getWithMetadata('key1',{});
    var expected = {
      value: 'value-key1',
      metadata: { someMetadataKey: 'someMetadataValue', someUnicodeMeta: '🤓' },
      cacheStatus: null
    };
    assert.deepEqual(response, expected);


    var response = await env.KV.getWithMetadata(['key1'],{});
    var expected = { key1: { metadata: 'example-metadata', value: '{"example":"values"}' } };
    assert.deepEqual(response, expected);


    var response = await env.KV.getWithMetadata(['key1'], "json");
    var expected = { key1: { metadata: 'example-metadata', value: { example: 'values' } } };
    assert.deepEqual(response, expected);

    var response = await env.KV.getWithMetadata(['key1', 'key2'], "json");
    var expected = { key1: { metadata: 'example-metadata', value: { example: 'values' } }, key2: { metadata: 'example-metadata', value: { example: 'values' } } };
    assert.deepEqual(response, expected);
  },
};
