// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export interface FetchHandler {
  fetch: (request: Request, env?: unknown, ctx?: unknown) => Promise<Response>;
}

export class PortTable {
  ephemeral(): number;
  tryBind(port: number, reusePort?: boolean): boolean;
  release(port: number): void;
  register(owner: object, port: number): void;
  unregister(owner: object): void;
  setHandler(port: number, handler: FetchHandler): void;
  getHandler(port: number): FetchHandler | undefined;
}

export const tcpPorts: PortTable;
