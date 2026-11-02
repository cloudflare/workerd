// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { connect } from 'cloudflare:sockets';

const udp = connect(
  { hostname: 'metrics.example.com', port: 8125 },
  { protocol: 'udp' }
);
udp.writable.getWriter().write(new Uint8Array([1, 2, 3]));
udp.close();

connect('metrics.example.com:8125', { protocol: 'tcp' });

// @ts-expect-error Socket protocols are limited to TCP and UDP.
connect('metrics.example.com:8125', { protocol: 'quic' });
