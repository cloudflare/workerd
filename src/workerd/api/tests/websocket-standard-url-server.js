// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The server ends, held so that none is garbage-collected, which would disconnect its client.
const serverEnds = [];

export default {
  fetch(request) {
    const [client, server] = Object.values(new WebSocketPair());
    serverEnds.push(server);
    server.accept();
    server.addEventListener('message', () => server.send(request.url));
    server.addEventListener('close', () => server.close());
    return new Response(null, { status: 101, webSocket: client });
  },
};
