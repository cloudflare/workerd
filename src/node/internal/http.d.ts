// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export interface FetchHandler {
  fetch: (request: Request, env?: unknown, ctx?: unknown) => Promise<Response>;
}

export interface InboundSocket {
  opened: Promise<{
    remoteAddress?: string | null;
    localAddress?: string | null;
  }>;
}

export interface ConnectHandler {
  connect: (
    socket: InboundSocket,
    env?: unknown,
    ctx?: unknown
  ) => Promise<void>;
}

export type PortHandler = FetchHandler | ConnectHandler;

export const GATEWAY_ADDRESS: string;

export class PortTable {
  readonly hostAddress: string;
  ephemeral(): number;
  declare(port: number): void;
  hasDeclared(): boolean;
  isDeclared(port: number): boolean;
  unclaimedDeclared(): number;
  tryBind(port: number, reusePort?: boolean): boolean;
  release(port: number): void;
  register(owner: object, port: number): void;
  unregister(owner: object): void;
  setHandler(port: number, handler: PortHandler): void;
  clearHandler(port: number, handler: PortHandler): void;
  getHandler(port: number): PortHandler | undefined;
}

export function tcpPorts(): PortTable;
export function lookupHandler(port: number): PortHandler | undefined;
