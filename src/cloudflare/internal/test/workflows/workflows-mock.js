// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request, env, ctx) {
    const data = await request.json();
    const reqUrl = new URL(request.url);

    if (reqUrl.pathname === '/get' && request.method === 'POST') {
      return Response.json(
        {
          result: {
            id: data.id,
          },
        },
        {
          status: 200,
          headers: {
            'content-type': 'application/json',
          },
        }
      );
    }

    if (reqUrl.pathname === '/create' && request.method === 'POST') {
      return Response.json(
        {
          result: {
            id: data.id,
          },
        },
        {
          status: 201,
          headers: {
            'content-type': 'application/json',
          },
        }
      );
    }

    if (reqUrl.pathname === '/createBatch' && request.method === 'POST') {
      return Response.json(
        {
          result: data.map((val) => ({ id: val.id })),
        },
        {
          status: 201,
          headers: {
            'content-type': 'application/json',
          },
        }
      );
    }
  },
};
