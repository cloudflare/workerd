// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request, env, ctx) {
    const data = await request.json();

    const modelName = request.headers.get('cf-consn-model-id');

    const respHeaders = {
      'cf-ai-req-id': '3a1983d7-1ddd-453a-ab75-c4358c91b582',
    };

    if (modelName === 'blobResponseModel') {
      let utf8Encode = new TextEncoder();
      utf8Encode.encode('hello world');

      return new Response(utf8Encode, {
        headers: respHeaders,
      });
    }

    if (modelName === 'rawInputs') {
      return Response.json(data, {
        headers: respHeaders,
      });
    }

    if (modelName === 'inputErrorModel') {
      return Response.json(
        {
          internalCode: 1001,
          message: 'InvalidInput: prompt and messages are mutually exclusive',
          name: 'InvalidInput',
          description: 'prompt and messages are mutually exclusive',
        },
        {
          status: 400,
          headers: {
            'content-type': 'application/json',
            ...respHeaders,
          },
        }
      );
    }

    return Response.json(
      { response: 'model response' },
      {
        headers: {
          'content-type': 'application/json',
          ...respHeaders,
        },
      }
    );
  },
};
