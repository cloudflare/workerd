// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export const tests = {
  async test(_, env) {
    {
      // Test toMarkdown with single file
      const resp = await env.ai.toMarkdown(
        {
          name: 'random.md',
          blob: new Blob([`# Random Markdown`], { type: 'text/markdown' }),
        },
        { gateway: { id: 'my-gateway' } }
      );

      assert.deepStrictEqual(resp, {
        name: 'random.md',
        mimeType: 'text/markdown',
        format: 'markdown',
        tokens: 0,
        data: '# Random Markdown',
      });
    }

    {
      // Test toMarkdown with extra headers
      const resp = await env.ai.toMarkdown(
        {
          name: 'headers.md',
          blob: new Blob([`# Random Markdown`], { type: 'text/markdown' }),
        },
        { extraHeaders: { example: 'header' } }
      );

      assert.deepStrictEqual(resp, {
        name: 'headers.md',
        mimeType: 'text/markdown',
        format: 'markdown',
        tokens: 0,
        data: { 'content-type': 'application/json', example: 'header' },
      });
    }

    {
      // Test toMarkdown with multiple file
      const resp = await env.ai.toMarkdown([
        {
          name: 'random.md',
          blob: new Blob([`# Random Markdown`], { type: 'text/markdown' }),
        },
        {
          name: 'cat.png',
          blob: new Blob(
            [
              `The image shows a white and orange cat standing on a beige floor`,
            ],
            { type: 'image/png' }
          ),
        },
      ]);

      assert.deepStrictEqual(resp, [
        {
          name: 'random.md',
          mimeType: 'text/markdown',
          format: 'markdown',
          tokens: 0,
          data: '# Random Markdown',
        },
        {
          name: 'cat.png',
          mimeType: 'image/png',
          format: 'markdown',
          tokens: 0,
          data: 'The image shows a white and orange cat standing on a beige floor',
        },
      ]);
    }

    {
      // Test toMarkdown using the exported service
      const resp = await env.ai.toMarkdown().transform([
        {
          name: 'random.md',
          blob: new Blob([`# Random Markdown`], { type: 'text/markdown' }),
        },
        {
          name: 'cat.png',
          blob: new Blob(
            [
              `The image shows a white and orange cat standing on a beige floor`,
            ],
            { type: 'image/png' }
          ),
        },
      ]);

      assert.deepStrictEqual(resp, [
        {
          name: 'random.md',
          mimeType: 'text/markdown',
          format: 'markdown',
          tokens: 0,
          data: '# Random Markdown',
        },
        {
          name: 'cat.png',
          mimeType: 'image/png',
          format: 'markdown',
          tokens: 0,
          data: 'The image shows a white and orange cat standing on a beige floor',
        },
      ]);
    }

    {
      // Test supported endpoint of ToMarkdown service
      const resp = await env.ai.toMarkdown().supported();

      assert.deepStrictEqual(resp, [
        {
          extension: '.md',
          mimeType: 'text/markdown',
        },
        {
          extension: '.png',
          mimeType: 'image/png',
        },
      ]);
    }
  },
};
