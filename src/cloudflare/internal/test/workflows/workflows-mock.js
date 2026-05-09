// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const restartBodies = new Map();

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

    if (reqUrl.pathname === '/restart' && request.method === 'POST') {
      restartBodies.set(data.id, data);
      return Response.json({}, { status: 200 });
    }

    // Test-only: returns the body from the last /restart call for a given id
    if (reqUrl.pathname === '/last-restart' && request.method === 'POST') {
      return Response.json(
        { result: restartBodies.get(data.id) ?? null },
        { status: 200 }
      );
    }
  },
};
