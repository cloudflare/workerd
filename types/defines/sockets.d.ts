// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare module "cloudflare:sockets" {
  function _connect(address: string | SocketAddress, options?: SocketOptions): Socket;
  export { _connect as connect };
}
