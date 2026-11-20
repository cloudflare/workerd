// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The service every beacon of the suite is sent to: the test workers' global
// outbound. It records each POST by its path, and reports one on
// GET /received?path=..., with 404 until it has arrived.

const beacons = new Map();

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (request.method === 'GET' && url.pathname === '/received') {
      const beacon = beacons.get(url.searchParams.get('path'));
      if (beacon === undefined) return new Response(null, { status: 404 });
      return Response.json(beacon);
    }
    const bytes = new Uint8Array(await request.arrayBuffer());
    beacons.set(url.pathname, {
      method: request.method,
      contentType: request.headers.get('content-type'),
      body: new TextDecoder().decode(bytes),
      size: bytes.length,
    });
    return new Response(null, { status: 204 });
  },
};
