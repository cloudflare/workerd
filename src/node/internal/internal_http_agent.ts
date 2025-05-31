// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import { ClientRequest } from 'node-internal:internal_http_client';
import { EventEmitter } from 'node-internal:events';
import {
  validateNumber,
  validateOneOf,
  validateString,
} from 'node-internal:validators';
import { isIP } from 'node-internal:internal_net';
import { once } from 'node-internal:internal_http_util';
import type { EventEmitter as EventEmitterType } from 'node:events';
import type { AgentOptions } from 'node:http';

const kOnKeylog = Symbol('onkeylog');
const kRequestOptions = Symbol('requestOptions');
const kRequestAsyncResource = Symbol('requestAsyncResource');

class ReusedHandle {
  type: string;
  handle: unknown;

  public constructor(type: string, handle: unknown) {
    this.type = type;
    this.handle = handle;
  }
}

function freeSocketErrorListener(this: unknown, _err: Error) {
  const socket = this;
  socket.destroy();
  socket.emit('agentRemove');
}

export type GetNameOptions = {
  host?: string;
  port?: number;
  localAddress?: string;
  family?: number;
  socketPath?: string;
};

export declare class Agent extends EventEmitterType {
  public defaultPort: number;
  public protocol: string;
  public options: AgentOptions & {
    path: string | null;
  };
  public requests: Record<string, ClientRequest[]>;
  public keepAliveMsecs: number;
  public keepAlive: boolean;
  public sockets: Record<string, unknown[]>;
  public freeSockets: Record<string, unknown[]>;
  public maxSockets: number;
  public maxFreeSockets: number;
  public scheduling: 'fifo' | 'lifo';
  public maxTotalSockets?: number | undefined;
  public totalSocketCount: number;
  public socketPath?: string | undefined;

  public constructor(options?: AgentOptions);

  public getName(options: GetNameOptions): string;
  public addRequest(
    req: ClientRequest,
    options:
      | string
      | { host?: string; port?: number; localAddress?: string; path?: string },
    port?: number | undefined /* legacy */,
    localAddress?: string | undefined /* legacy */
  ): void;
  public keepSocketAlive(socket: unknown): boolean;
  public removeSocket(s: unknown, options: Record<string, unknown>): void;
  public reuseSocket(socket: unknown, req: ClientRequest): void;
  public destroy(): void;
}

// @ts-expect-error TS2323 Redeclare error.
export function Agent(this: Agent, options?: AgentOptions) {
  if (!(this instanceof Agent)) return new Agent(options);

  EventEmitter.call(this as unknown as EventEmitter);

  this.defaultPort = 80;
  this.protocol = 'http:';
  // @ts-expect-error TS2353 It's ok to use __proto__
  this.options = { __proto__: null, ...options };

  if (this.options.noDelay === undefined) this.options.noDelay = true;

  // Don't confuse net and make it think that we're connecting to a pipe
  this.options.path = null;
  this.requests = Object.create(null);
  this.sockets = Object.create(null);
  this.freeSockets = Object.create(null);
  this.keepAliveMsecs = this.options.keepAliveMsecs || 1000;
  this.keepAlive = this.options.keepAlive || false;
  this.maxSockets = this.options.maxSockets || Agent.defaultMaxSockets;
  this.maxFreeSockets = this.options.maxFreeSockets || 256;
  this.scheduling = this.options.scheduling || 'lifo';
  this.maxTotalSockets = this.options.maxTotalSockets;
  this.totalSocketCount = 0;

  validateOneOf(this.scheduling, 'scheduling', ['fifo', 'lifo']);

  if (this.maxTotalSockets !== undefined) {
    validateNumber(this.maxTotalSockets, 'maxTotalSockets', 1);
  } else {
    this.maxTotalSockets = Infinity;
  }

  this.on('free', (socket, options) => {
    const name = this.getName(options);

    // TODO(ronag): socket.destroy(err) might have been called
    // before coming here and have an 'error' scheduled. In the
    // case of socket.destroy() below this 'error' has no handler
    // and could cause unhandled exception.

    if (!socket.writable) {
      socket.destroy();
      return;
    }

    const requests = this.requests[name];
    if (requests?.length) {
      const req = requests.shift();
      const reqAsyncRes = req[kRequestAsyncResource];
      if (reqAsyncRes) {
        // Run request within the original async context.
        reqAsyncRes.runInAsyncScope(() => {
          asyncResetHandle(socket);
          setRequestSocket(this, req, socket);
        });
        req[kRequestAsyncResource] = null;
      } else {
        setRequestSocket(this, req, socket);
      }
      if (requests.length === 0) {
        delete this.requests[name];
      }
      return;
    }

    // If there are no pending requests, then put it in
    // the freeSockets pool, but only if we're allowed to do so.
    const req = socket._httpMessage;
    if (!req || !req.shouldKeepAlive || !this.keepAlive) {
      socket.destroy();
      return;
    }

    const freeSockets = this.freeSockets[name] || [];
    const freeLen = freeSockets.length;
    let count = freeLen;
    if (this.sockets[name]) count += this.sockets[name].length;

    if (
      this.totalSocketCount > this.maxTotalSockets ||
      count > this.maxSockets ||
      freeLen >= this.maxFreeSockets ||
      !this.keepSocketAlive(socket)
    ) {
      socket.destroy();
      return;
    }

    this.freeSockets[name] = freeSockets;
    socket[async_id_symbol] = -1;
    socket._httpMessage = null;
    this.removeSocket(socket, options);

    socket.once('error', freeSocketErrorListener);
    freeSockets.push(socket);
  });

  // Don't emit keylog events unless there is a listener for them.
  this.on('newListener', maybeEnableKeylog);
}
Object.setPrototypeOf(Agent.prototype, EventEmitter.prototype);
Object.setPrototypeOf(Agent, EventEmitter);

function maybeEnableKeylog(this: Agent, eventName: string): void {
  if (eventName === 'keylog') {
    this.removeListener('newListener', maybeEnableKeylog);
    // Future sockets will listen on keylog at creation.
    const agent = this;
    this[kOnKeylog] = function onkeylog(keylog) {
      agent.emit('keylog', keylog, this);
    };
    // Existing sockets will start listening on keylog now.
    const sockets = Object.values(this.sockets);
    for (let i = 0; i < sockets.length; i++) {
      sockets[i].on('keylog', this[kOnKeylog]);
    }
  }
}

Agent.defaultMaxSockets = Infinity;

// Agent.prototype.createConnection = net.createConnection;

// Get the key for a given set of request options
Agent.prototype.getName = function getName(
  options: GetNameOptions = {}
): string {
  let name = options.host || 'localhost';

  name += ':';
  if (options.port) name += options.port;

  name += ':';
  if (options.localAddress) name += options.localAddress;

  // Pacify parallel/test-http-agent-getname by only appending
  // the ':' when options.family is set.
  if (options.family === 4 || options.family === 6)
    name += `:${options.family}`;

  if (options.socketPath) name += `:${options.socketPath}`;

  return name;
};

Agent.prototype.addRequest = function addRequest(
  req: ClientRequest,
  options:
    | string
    | { host?: string; port?: number; localAddress?: string; path?: string },
  port?: number | undefined /* legacy */,
  localAddress?: string | undefined /* legacy */
): void {
  // Legacy API: addRequest(req, host, port, localAddress)
  if (typeof options === 'string') {
    options = {
      // @ts-expect-error TS2353 It's ok to use __proto__
      __proto__: null,
      host: options,
      port,
      localAddress,
    };
  }

  // @ts-expect-error TS2353 It's ok to use __proto__
  options = { __proto__: null, ...options, ...this.options };
  if (options.socketPath) options.path = options.socketPath;

  normalizeServerName(options, req);

  const name = this.getName(options);
  this.sockets[name] ||= [];

  const freeSockets = this.freeSockets[name];
  let socket;
  if (freeSockets) {
    while (freeSockets.length && freeSockets[0].destroyed) {
      freeSockets.shift();
    }
    socket =
      this.scheduling === 'fifo' ? freeSockets.shift() : freeSockets.pop();
    if (!freeSockets.length) delete this.freeSockets[name];
  }

  const freeLen = freeSockets ? freeSockets.length : 0;
  const sockLen = freeLen + this.sockets[name].length;

  if (socket) {
    asyncResetHandle(socket);
    this.reuseSocket(socket, req);
    setRequestSocket(this, req, socket);
    this.sockets[name].push(socket);
  } else if (
    sockLen < this.maxSockets &&
    this.totalSocketCount < this.maxTotalSockets
  ) {
    // If we are under maxSockets create a new one.
    this.createSocket(req, options, (err, socket) => {
      if (err) req.onSocket(socket, err);
      else setRequestSocket(this, req, socket);
    });
  } else {
    // We are over limit so we'll add it to the queue.
    this.requests[name] ||= [];

    // Used to create sockets for pending requests from different origin
    req[kRequestOptions] = options;
    // Used to capture the original async context.
    req[kRequestAsyncResource] = new AsyncResource('QueuedRequest');

    this.requests[name].push(req);
  }
};

Agent.prototype.createSocket = function createSocket(
  req: ClientRequest,
  options: Record<string, unknown>,
  cb: (error?: Error | null) => void
) {
  options = { __proto__: null, ...options, ...this.options };
  if (options.socketPath) options.path = options.socketPath;

  normalizeServerName(options, req);

  const name = this.getName(options);
  options._agentKey = name;

  options.encoding = null;

  const oncreate = once((err, s) => {
    if (err) return cb(err);
    this.sockets[name] ??= [];
    this.sockets[name]?.push(s);
    this.totalSocketCount++;
    installListeners(this, s, options);
    cb(null, s);
  });
  // When keepAlive is true, pass the related options to createConnection
  if (this.keepAlive) {
    options.keepAlive = this.keepAlive;
    options.keepAliveInitialDelay = this.keepAliveMsecs;
  }
  const newSocket = this.createConnection(options, oncreate);
  if (newSocket) oncreate(null, newSocket);
};

function normalizeServerName(
  options: { servername?: string; host: string },
  req: ClientRequest
): void {
  if (!options.servername && options.servername !== '')
    options.servername = calculateServerName(options, req);
}

function calculateServerName(
  options: { host: string },
  req: ClientRequest
): string {
  let servername = options.host;
  const hostHeader = req.getHeader('host');
  if (hostHeader) {
    validateString(hostHeader, 'options.headers.host');

    // abc => abc
    // abc:123 => abc
    // [::1] => ::1
    // [::1]:123 => ::1
    if (hostHeader[0] === '[') {
      const index = hostHeader.indexOf(']');
      if (index === -1) {
        // Leading '[', but no ']'. Need to do something...
        servername = hostHeader;
      } else {
        servername = hostHeader.substring(1, index);
      }
    } else {
      servername = hostHeader.split(':', 1)[0] as string;
    }
  }
  // Don't implicitly set invalid (IP) servernames.
  if (isIP(servername)) servername = '';
  return servername;
}

function installListeners(
  agent: Agent,
  s: unknown,
  options: Record<string, unknown>
): void {
  function onFree() {
    agent.emit('free', s, options);
  }
  s.on('free', onFree);

  function onClose(_err: Error) {
    // This is the only place where sockets get removed from the Agent.
    // If you want to remove a socket from the pool, just close it.
    // All socket errors end in a close event anyway.
    agent.totalSocketCount--;
    agent.removeSocket(s, options);
  }
  s.on('close', onClose);

  function onTimeout(): void {
    // Destroy if in free list.
    // TODO(ronag): Always destroy, even if not in free list.
    const sockets = agent.freeSockets;
    if (Object.keys(sockets).some((name) => sockets[name].includes(s))) {
      return s.destroy();
    }
  }
  s.on('timeout', onTimeout);

  function onRemove(): void {
    // We need this function for cases like HTTP 'upgrade'
    // (defined by WebSockets) where we need to remove a socket from the
    // pool because it'll be locked up indefinitely
    agent.totalSocketCount--;
    agent.removeSocket(s, options);
    s.removeListener('close', onClose);
    s.removeListener('free', onFree);
    s.removeListener('timeout', onTimeout);
    s.removeListener('agentRemove', onRemove);
  }
  s.on('agentRemove', onRemove);

  if (agent[kOnKeylog]) {
    s.on('keylog', agent[kOnKeylog]);
  }
}

Agent.prototype.removeSocket = function removeSocket(
  s: unknown,
  options: Record<string, unknown>
): void {
  const name = this.getName(options);
  const sets = [this.sockets];

  // If the socket was destroyed, remove it from the free buffers too.
  if (!s.writable) sets.push(this.freeSockets);

  for (let sk = 0; sk < sets.length; sk++) {
    const sockets = sets[sk];

    if (sockets[name]) {
      const index = sockets[name].indexOf(s);
      if (index !== -1) {
        sockets[name].splice(index, 1);
        // Don't leak
        if (sockets[name].length === 0) delete sockets[name];
      }
    }
  }

  let req: ClientRequest | undefined;
  if (this.requests[name]?.length) {
    req = this.requests[name]?.at(0);
  } else {
    // TODO(rickyes): this logic will not be FIFO across origins.
    // There might be older requests in a different origin, but
    // if the origin which releases the socket has pending requests
    // that will be prioritized.
    const keys = Object.keys(this.requests);
    for (let i = 0; i < keys.length; i++) {
      const prop = keys[i] as string;
      // Check whether this specific origin is already at maxSockets
      if (this.sockets[prop]?.length) break;
      req = this.requests[prop]?.at(0);
      options = req[kRequestOptions];
      break;
    }
  }

  if (req && options) {
    req[kRequestOptions] = undefined;
    // If we have pending requests and a socket gets closed make a new one
    this.createSocket(req, options, (err: Error, socket) => {
      if (err) req.onSocket(socket, err);
      else socket.emit('free');
    });
  }
};

Agent.prototype.keepSocketAlive = function keepSocketAlive(
  _socket: unknown
): boolean {
  // We don't support keepSocketAlive option
  return false;
};

Agent.prototype.reuseSocket = function reuseSocket(
  _socket: unknown,
  _req: ClientRequest
): void {
  // Do nothing
};

Agent.prototype.destroy = function destroy(): void {
  const sets = [this.freeSockets, this.sockets];
  for (let s = 0; s < sets.length; s++) {
    const set = sets[s];
    const keys = Object.keys(set);
    for (let v = 0; v < keys.length; v++) {
      const setName = set[keys[v]];
      for (let n = 0; n < setName.length; n++) {
        setName[n].destroy();
      }
    }
  }
};

function setRequestSocket(
  agent: Agent,
  req: ClientRequest,
  socket: unknown
): void {
  req.onSocket(socket);
  const agentTimeout = agent.options.timeout || 0;
  if (req.timeout === undefined || req.timeout === agentTimeout) {
    return;
  }
  socket.setTimeout(req.timeout);
}

function asyncResetHandle(socket: unknown): void {
  // Guard against an uninitialized or user supplied Socket.
  const handle = socket._handle;
  if (handle && typeof handle.asyncReset === 'function') {
    // Assign the handle a new asyncId and run any destroy()/init() hooks.
    handle.asyncReset(new ReusedHandle(handle.getProviderType(), handle));
    socket[async_id_symbol] = handle.getAsyncId();
  }
}

export const globalAgent = new Agent({
  keepAlive: true,
  scheduling: 'lifo',
  timeout: 5000,
});
