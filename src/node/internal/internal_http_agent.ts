// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import { EventEmitter } from 'node-internal:events';
import { validateOneOf, validateNumber } from 'node-internal:validators';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import type {
  RequestOptions,
  Agent as _Agent,
  AgentOptions,
  ClientRequest,
  IncomingMessage,
} from 'node:http';
import type { Socket, NetConnectOpts } from 'node:net';
import type { Duplex } from 'node:stream';

// This is mostly a stub implementation.
// We don't intend to support the Agent API right now beyond providing a very limited stub API.
//
export class Agent extends EventEmitter implements _Agent {
  defaultPort = 80;
  protocol: string = 'http:';

  options: AgentOptions & { __proto__: null };
  keepAliveMsecs: number = 1000;
  keepAlive: boolean = false;
  maxSockets: number;
  maxFreeSockets: number;
  scheduling: 'lifo' | 'fifo' = 'lifo';
  maxTotalSockets: number;
  totalSocketCount: number;
  readonly freeSockets: NodeJS.ReadOnlyDict<Socket[]> = {};
  readonly sockets: NodeJS.ReadOnlyDict<Socket[]> = {};
  readonly requests: NodeJS.ReadOnlyDict<IncomingMessage[]> = {};

  constructor(options?: AgentOptions) {
    super({});
    this.options = { __proto__: null, ...options };

    if (this.options.noDelay === undefined) this.options.noDelay = true;

    // Don't confuse net and make it think that we're connecting to a pipe
    this.keepAliveMsecs = this.options.keepAliveMsecs || 1000;
    this.keepAlive = this.options.keepAlive || false;
    this.maxSockets = this.options.maxSockets || Agent.defaultMaxSockets;
    this.maxFreeSockets = this.options.maxFreeSockets || 256;
    this.scheduling = this.options.scheduling || 'lifo';
    this.totalSocketCount = 0;

    validateOneOf(this.scheduling, 'scheduling', ['fifo', 'lifo']);

    if (this.options.maxTotalSockets !== undefined) {
      validateNumber(this.options.maxTotalSockets, 'maxTotalSockets', 1);
      this.maxTotalSockets = this.options.maxTotalSockets;
    } else {
      this.maxTotalSockets = Infinity;
    }
  }

  static defaultMaxSockets: number = Infinity;

  createConnection(
    _options: NetConnectOpts,
    _callback?: (err: Error | null, stream: Duplex) => void
  ): Duplex {
    // We can't create a connection like this because our implementation
    // relies on fetch() not on node:net sockets. In node.js, method
    // calls node:net's createConnection method.
    throw new ERR_METHOD_NOT_IMPLEMENTED('createConnection');
  }

  getName(options: RequestOptions = {}): string {
    let name = options.host || 'localhost';

    name += ':';
    if (options.port) name += options.port.toString();

    name += ':';
    if (options.localAddress) name += options.localAddress;

    // Pacify parallel/test-http-agent-getname by only appending
    // the ':' when options.family is set.
    if (options.family === 4 || options.family === 6)
      name += `:${options.family}`;

    if (options.socketPath) name += `:${options.socketPath}`;

    return name;
  }

  addRequest(_req: ClientRequest, _options: unknown): void {
    // Not implemented. Acts as a no-op.
  }

  createSocket(
    _req: ClientRequest,
    _options: unknown,
    _callback: VoidFunction
  ): void {
    // Not implemented. Acts as a no-op.
  }

  removeSocket(_socket: Socket): void {
    // Not implemented. Acts as a no-op.
  }

  keepSocketAlive(_socket: Socket): boolean {
    // Not implemented. Acts as a no-op.
    return false;
  }

  reuseSocket(_socket: Socket, _req: ClientRequest): void {
    // Not implemented. Acts as a no-op.
  }

  destroy(_error?: Error): void {
    // Not implemented. Acts as a no-op.
  }
}

export const globalAgent = new Agent({
  keepAlive: true,
  scheduling: 'lifo',
  timeout: 5000,
});
