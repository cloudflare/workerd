// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A node:dgram echo server on this worker's one declared UDP listener. Each
// datagram is answered with `<address>:<port>:<family>:<size>:` followed by the
// payload, so the driver can check rinfo from the wire.

import { createSocket } from 'node:dgram';
import { connectHandler } from 'cloudflare:node';

const socket = createSocket('udp4');
socket.on('message', (msg, rinfo) => {
  const header = Buffer.from(
    `${rinfo.address}:${rinfo.port}:${rinfo.family}:${rinfo.size}:`
  );
  socket.send([header, msg], rinfo.port, rinfo.address);
});
socket.bind();

export default connectHandler();
