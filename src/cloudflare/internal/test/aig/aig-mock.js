// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request, env, ctx) {
    if (request.method === 'GET') {
      if (request.url.endsWith('logs/404')) {
        return Response.json(
          {
            errors: [
              {
                code: 7002,
                message: 'Not Found',
              },
            ],
            success: true,
          },
          { status: 404 }
        );
      }

      if (request.url.endsWith('url/openai')) {
        return Response.json({
          result: {
            url: 'https://gateway.ai.cloudflare.com/v1/account-tag-abc/my-gateway/openai',
          },
          success: true,
        });
      }

      if (request.url.endsWith('logs/500')) {
        return Response.json(
          {
            errors: [
              {
                code: 7000,
                message: 'Internal Error',
              },
            ],
            success: true,
          },
          { status: 500 }
        );
      }

      return Response.json({
        success: true,
        result: {
          cached: false,
          cost: 0,
          created_at: '2019-08-24T14:15:22Z',
          custom_cost: true,
          duration: 0,
          id: 'string',
          metadata: 'string',
          model: 'string',
          model_type: 'string',
          path: 'string',
          provider: 'string',
          request_content_type: 'string',
          request_head: 'string',
          request_head_complete: true,
          request_size: 0,
          request_type: 'string',
          response_content_type: 'string',
          response_head: 'string',
          response_head_complete: true,
          response_size: 0,
          status_code: 0,
          step: 0,
          success: true,
          tokens_in: 0,
          tokens_out: 0,
        },
      });
    }

    if (request.method === 'PATCH') {
      if (request.url.endsWith('logs/404')) {
        return Response.json(
          {
            errors: [
              {
                code: 7002,
                message: 'Not Found',
              },
            ],
            success: true,
          },
          { status: 404 }
        );
      }

      if (request.url.endsWith('logs/500')) {
        return Response.json(
          {
            errors: [
              {
                code: 7002,
                message: 'Internal Error',
              },
            ],
            success: true,
          },
          { status: 500 }
        );
      }

      return Response.json({ success: true });
    }

    if (request.method === 'POST') {
      const body = await request.json();

      return Response.json({ success: true, result: body });
    }

    return Response.json({ success: false }, { status: 500 });
  },
};
