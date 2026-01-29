// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const method = request.method;

    // List instances: GET /ai-search/instances
    if (method === 'GET' && url.pathname === '/ai-search/instances') {
      return Response.json({
        success: true,
        result: [
          {
            id: 'my-ai-search',
            enable: true,
            type: 'r2',
            source: 'my-bucket',
          },
        ],
      });
    }

    // Create instance: POST /ai-search/instances
    if (method === 'POST' && url.pathname === '/ai-search/instances') {
      const body = await request.json();
      return Response.json({
        success: true,
        result: {
          id: body.id,
          type: body.type,
          source: body.source,
        },
      });
    }

    // Delete instance: DELETE /ai-search/instances/{id}
    if (
      method === 'DELETE' &&
      url.pathname.match(/^\/ai-search\/instances\/[^/]+$/)
    ) {
      return Response.json({
        success: true,
        result: {},
      });
    }

    // Search: POST /ai-search/instances/{id}/search
    if (method === 'POST' && url.pathname.includes('/search')) {
      // Check if instance ID is non-existent
      if (url.pathname.includes('/non-existent/')) {
        return Response.json(
          {
            success: false,
            errors: [{ message: 'AI Search instance not found' }],
          },
          { status: 404 }
        );
      }

      return Response.json({
        success: true,
        result: {
          search_query: 'How many woodchucks?',
          chunks: [
            {
              id: 'chunk-1',
              type: 'text',
              score: 0.85,
              text: 'Two woodchucks per passenger.',
              item: {
                timestamp: 1234567890,
                key: 'woodchuck_policy.txt',
                metadata: {
                  region: 'North America',
                  author: 'Wildlife Department',
                },
              },
              scoring_details: {
                keyword_score: 0.8,
                vector_score: 0.9,
              },
            },
          ],
        },
      });
    }

    // Chat Completions: POST /ai-search/instances/{id}/chat/completions
    if (method === 'POST' && url.pathname.includes('/chat/completions')) {
      // Check if instance ID is non-existent
      if (url.pathname.includes('/non-existent/')) {
        return Response.json(
          {
            success: false,
            errors: [{ message: 'AI Search instance not found' }],
          },
          { status: 404 }
        );
      }

      const body = await request.json();

      // Return streaming response if stream=true
      if (body.stream === true) {
        return new Response('data: {"result": "streaming"}\n\n', {
          headers: {
            'content-type': 'text/event-stream',
          },
        });
      }

      // Return non-streaming response
      return Response.json({
        success: true,
        result: {
          response: 'Based on the documents, two woodchucks are allowed.',
        },
      });
    }

    // Default: 404
    return Response.json(
      {
        success: false,
        errors: [{ message: 'Not found' }],
      },
      { status: 404 }
    );
  },
};
