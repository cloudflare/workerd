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
import type { Socket } from 'node:net';

// @ts-expect-error TS2507 EventEmitter is not a constructor function type.
export class Agent extends EventEmitter implements _Agent {
  public defaultPort = 80;
  public protocol: string = 'http:';

  public options: AgentOptions & { __proto__: null };
  public keepAliveMsecs: number = 1000;
  public keepAlive: boolean = false;
  public maxSockets: number;
  public maxFreeSockets: number;
  public scheduling: 'lifo' | 'fifo' = 'lifo';
  public maxTotalSockets: number;
  public totalSocketCount: number;
  public readonly freeSockets: NodeJS.ReadOnlyDict<Socket[]> = {};
  public readonly sockets: NodeJS.ReadOnlyDict<Socket[]> = {};
  public readonly requests: NodeJS.ReadOnlyDict<IncomingMessage[]> = {};

  public constructor(options?: AgentOptions) {
    super(); // eslint-disable-line @typescript-eslint/no-unsafe-call
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

  public static defaultMaxSockets: number = Infinity;

  public createConnection(): void {
    // We can't create a connection like this because our implementation
    // relies on fetch() not on node:net sockets. In node.js, method
    // calls node:net's createConnection method.
    throw new ERR_METHOD_NOT_IMPLEMENTED('createConnection');
  }

  public getName(options: RequestOptions = {}): string {
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

  public addRequest(_req: ClientRequest, _options: unknown): void {
    // Not implemented. Acts as a no-op.
  }

  public createSocket(
    _req: ClientRequest,
    _options: unknown,
    _callback: VoidFunction
  ): void {
    // Not implemented. Acts as a no-op.
  }

  public removeSocket(_socket: Socket): void {
    // Not implemented. Acts as a no-op.
  }

  public keepSocketAlive(_socket: Socket): boolean {
    // Not implemented. Acts as a no-op.
    return false;
  }

  public reuseSocket(_socket: Socket, _req: ClientRequest): void {
    // Not implemented. Acts as a no-op.
  }

  public destroy(_error?: Error): void {
    // Not implemented. Acts as a no-op.
  }
}

export const globalAgent = new Agent({
  keepAlive: true,
  scheduling: 'lifo',
  timeout: 5000,
});
