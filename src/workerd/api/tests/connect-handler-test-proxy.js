// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class ConnectProxy extends WorkerEntrypoint {
  async connect(socket) {
    // proxy for ConnectEndpoint instance on port 8083.
    let upstream = connect('localhost:8083');
    await Promise.all([
      socket.readable.pipeTo(upstream.writable),
      upstream.readable.pipeTo(socket.writable),
    ]);
  }
}

export class ConnectEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    const enc = new TextEncoder();
    let writer = socket.writable.getWriter();
    await writer.write(enc.encode('hello-from-endpoint'));
    await writer.close();
  }
}

// Reached via a service binding from connect-handler-test.js. Awaits socket.opened and echoes back
// the observed localAddress, which on the service-binding path is the verbatim authority string
// the caller passed to fetcher.connect(...). The client test asserts strict equality.
export class LocalAddressEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    const { localAddress } = await socket.opened;
    const enc = new TextEncoder();
    const writer = socket.writable.getWriter();
    await writer.write(enc.encode(`OK:${localAddress}`));
    await writer.close();
  }
}
