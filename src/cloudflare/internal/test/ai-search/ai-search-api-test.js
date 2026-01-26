// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const tests = {
  async test(_, env) {
    // Test list()
    {
      const instances = await env.ai.aiSearch.list();
      assert.ok(Array.isArray(instances));
      assert.equal(instances.length, 1);
      assert.equal(instances[0].id, 'my-ai-search');
      assert.equal(instances[0].type, 'r2');
      assert.equal(instances[0].source, 'my-bucket');
    }

    // Test search()
    {
      const searchResp = await env.ai.aiSearch.get('my-ai-search').search({
        messages: [{ role: 'user', content: 'How many woodchucks?' }],
        ai_search_options: {
          retrieval: {
            max_num_results: 10,
            match_threshold: 0.4,
          },
        },
      });

      assert.equal(searchResp.search_query, 'How many woodchucks?');
      assert.ok(Array.isArray(searchResp.chunks));
      assert.equal(searchResp.chunks.length, 1);
      assert.equal(searchResp.chunks[0].id, 'chunk-1');
      assert.equal(searchResp.chunks[0].type, 'text');
      assert.equal(searchResp.chunks[0].score, 0.85);
      assert.equal(searchResp.chunks[0].text, 'Two woodchucks per passenger.');
      assert.equal(searchResp.chunks[0].item.key, 'woodchuck_policy.txt');
      assert.equal(searchResp.chunks[0].item.metadata.region, 'North America');
      assert.equal(
        searchResp.chunks[0].item.metadata.author,
        'Wildlife Department'
      );
      assert.equal(searchResp.chunks[0].scoring_details.keyword_score, 0.8);
      assert.equal(searchResp.chunks[0].scoring_details.vector_score, 0.9);
    }

    // Test chatCompletions() - non-streaming
    {
      const chatResp = await env.ai.aiSearch
        .get('my-ai-search')
        .chatCompletions({
          messages: [{ role: 'user', content: 'How many woodchucks?' }],
          model: '@cf/meta/llama-3.3-70b-instruct-fp8-fast',
          stream: false,
          ai_search_options: {
            retrieval: {
              max_num_results: 5,
            },
          },
        });

      assert.ok(chatResp);
      assert.ok(chatResp.response);
      assert.equal(
        chatResp.response,
        'Based on the documents, two woodchucks are allowed.'
      );
    }

    // Test chatCompletions() - streaming
    {
      const chatStreamResp = await env.ai.aiSearch
        .get('my-ai-search')
        .chatCompletions({
          messages: [{ role: 'user', content: 'How many woodchucks?' }],
          stream: true,
        });

      assert.ok(chatStreamResp instanceof Response);
      assert.equal(
        chatStreamResp.headers.get('content-type'),
        'text/event-stream'
      );
    }

    // Test create()
    {
      const created = await env.ai.aiSearch.create({
        id: 'new-search',
        type: 'r2',
        source: 'my-new-bucket',
      });

      assert.ok(created);
    }

    // Test delete()
    {
      await env.ai.aiSearch.get('my-ai-search').delete();
      // If no error is thrown, the test passes
      assert.ok(true);
    }

    // Test error handling - instance not found
    {
      try {
        await env.ai.aiSearch.get('non-existent').search({
          messages: [{ role: 'user', content: 'test' }],
        });
        assert.fail('Should have thrown an error');
      } catch (error) {
        assert.ok(error instanceof Error);
        assert.equal(error.name, 'AiSearchNotFoundError');
      }
    }
  },
};
