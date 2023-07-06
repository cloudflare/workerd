// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO: c++ built-ins do not yet support named exports
import sockets from 'cloudflare-internal:sockets';
export function connect(
  address: string | sockets.SocketAddress, options: sockets.SocketOptions
): sockets.Socket {
  return sockets.connect(address, options);
}
