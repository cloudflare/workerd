// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
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

import {
  udpPorts,
  GATEWAY_ADDRESS,
  type ConnectHandler,
  type InboundSocket,
} from 'cloudflare-internal:http';
import { EventEmitter } from 'node-internal:events';
import { Buffer } from 'node-internal:internal_buffer';
import { nextTick } from 'node-internal:internal_process';
import { isIP, isIPv4, parseAuthority } from 'node-internal:internal_net';
import {
  validateAbortSignal,
  validateNumber,
  validatePort,
  validateString,
} from 'node-internal:validators';
import {
  EADDRINUSE,
  EADDRNOTAVAIL,
  EBADF,
  EHOSTUNREACH,
  ERR_BUFFER_OUT_OF_BOUNDS,
  ERR_FEATURE_UNAVAILABLE_ON_PLATFORM,
  ERR_INVALID_ARG_TYPE,
  ERR_MISSING_ARGS,
  ERR_SOCKET_ALREADY_BOUND,
  ERR_SOCKET_BAD_BUFFER_SIZE,
  ERR_SOCKET_BAD_TYPE,
  ERR_SOCKET_DGRAM_IS_CONNECTED,
  ERR_SOCKET_DGRAM_NOT_CONNECTED,
  ERR_SOCKET_DGRAM_NOT_RUNNING,
} from 'node-internal:internal_errors';

import type {
  BindOptions,
  RemoteInfo,
  SocketOptions,
  SocketType,
} from 'node:dgram';
import type { AddressInfo } from 'node:net';

// The chunk type carried by a UDP platform socket's value-mode streams.
interface DatagramChunk {
  readonly data: Uint8Array;
}

declare const Datagram: new (data: Uint8Array) => DatagramChunk;

// A UDP platform socket delivered to connect(): one flow of datagrams to and
// from a single peer.
type UdpInboundSocket = InboundSocket & {
  readable: ReadableStream<DatagramChunk>;
  writable: WritableStream<DatagramChunk>;
  close(): Promise<void>;
};

type Flow = {
  reader: ReadableStreamDefaultReader<DatagramChunk>;
  writer: WritableStreamDefaultWriter<DatagramChunk>;
};

type SendCallback = (error: Error | null, bytes?: number) => void;

const DEFAULT_IPV4_ADDR = '0.0.0.0';
const DEFAULT_IPV6_ADDR = '::';

function stripMappedV4(address: string): string | undefined {
  const lower = address.toLowerCase();
  if (lower.startsWith('::ffff:') && isIPv4(lower.slice(7))) {
    return lower.slice(7);
  }
  return undefined;
}

// Peers are keyed by their IPv4 form when they have one, so that a v4-mapped
// peer on a dual-stack listener matches send(msg, port, '1.2.3.4').
function flowKey(address: string, port: number): string {
  const v4 = stripMappedV4(address);
  return `${v4 ?? address.toLowerCase()}:${port}`;
}

function sliceBuffer(
  buffer: unknown,
  offset: unknown,
  length: unknown
): Uint8Array {
  if (typeof buffer === 'string') {
    buffer = Buffer.from(buffer);
  } else if (!ArrayBuffer.isView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      'buffer',
      ['Buffer', 'TypedArray', 'DataView', 'string'],
      buffer
    );
  }
  const view = buffer as ArrayBufferView;
  offset = (offset as number) >>> 0;
  length = (length as number) >>> 0;
  if ((offset as number) > view.byteLength) {
    throw new ERR_BUFFER_OUT_OF_BOUNDS('offset');
  }
  if ((length as number) > view.byteLength - (offset as number)) {
    throw new ERR_BUFFER_OUT_OF_BOUNDS('length');
  }
  return new Uint8Array(
    view.buffer,
    view.byteOffset + (offset as number),
    length as number
  );
}

function fixBufferList(list: unknown[]): Uint8Array[] | null {
  const newlist: Uint8Array[] = new Array<Uint8Array>(list.length);
  for (let i = 0, l = list.length; i < l; i++) {
    const buf = list[i];
    if (typeof buf === 'string') {
      newlist[i] = Buffer.from(buf);
    } else if (!ArrayBuffer.isView(buf)) {
      return null;
    } else {
      newlist[i] = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
    }
  }
  return newlist;
}

function toBuffer(list: Uint8Array[]): Buffer {
  if (list.length === 1) {
    const [only] = list as [Uint8Array];
    return Buffer.from(only.buffer, only.byteOffset, only.byteLength);
  }
  return Buffer.concat(list) as Buffer;
}

// A dgram.Socket is the isolate-level owner of a UDP port: bind() claims the
// port in the UDP port table and installs a connect handler there, and every
// inbound platform flow routed to it (see connectHandler in cloudflare:node)
// becomes a peer whose datagrams are emitted as 'message' events. send() is
// only deliverable to a peer with a live flow, since the platform offers no
// unconnected egress; datagrams to anyone else fail with EHOSTUNREACH.
export class Socket extends EventEmitter {
  type: SocketType;

  #address: AddressInfo | null = null;
  #remote: AddressInfo | null = null;
  #closed = false;
  #reuse: boolean;
  #recvBufferSize = 0;
  #sendBufferSize = 0;
  #flows = new Map<string, Flow>();
  #handler: ConnectHandler = {
    connect: (socket: InboundSocket): Promise<void> =>
      this.#onFlow(socket as UdpInboundSocket),
  };

  constructor(type: unknown, listener?: unknown) {
    super();
    let options: SocketOptions | undefined;
    if (type !== null && typeof type === 'object') {
      options = type as SocketOptions;
      type = options.type;
    }
    if (type !== 'udp4' && type !== 'udp6') {
      throw new ERR_SOCKET_BAD_TYPE();
    }
    this.type = type;
    this.#reuse = Boolean(options?.reuseAddr) || Boolean(options?.reusePort);

    if (options?.recvBufferSize !== undefined) {
      this.setRecvBufferSize(options.recvBufferSize);
    }
    if (options?.sendBufferSize !== undefined) {
      this.setSendBufferSize(options.sendBufferSize);
    }
    if (options?.signal !== undefined) {
      const { signal } = options;
      validateAbortSignal(signal, 'options.signal');
      const onAborted = (): void => {
        if (!this.#closed) this.close();
      };
      if (signal.aborted) {
        onAborted();
      } else {
        signal.addEventListener('abort', onAborted, { once: true });
        this.once('close', () => {
          signal.removeEventListener('abort', onAborted);
        });
      }
    }

    if (typeof listener === 'function') {
      this.on('message', listener as (...args: unknown[]) => void);
    }
  }

  #healthCheck(): void {
    if (this.#closed) {
      throw new ERR_SOCKET_DGRAM_NOT_RUNNING();
    }
  }

  #loopback(): string {
    return this.type === 'udp4' ? '127.0.0.1' : '::1';
  }

  // Claims a port. An explicit bind() takes a declared listener port where the
  // platform declares them; a bind implied by send() or connect() takes an
  // ephemeral port, since egress-only use must not consume a listener.
  #bind(port: number, address: string, implicit: boolean): void {
    if (address === 'localhost') address = this.#loopback();
    if (!implicit && udpPorts.hasDeclared()) {
      if (port === 0) {
        port = udpPorts.unclaimedDeclared();
        if (port === 0) throw new EADDRINUSE(address, 0);
      } else if (!udpPorts.isDeclared(port)) {
        throw new EADDRNOTAVAIL(address, port);
      }
    }
    if (port === 0) {
      port = udpPorts.ephemeral();
      if (port === 0) throw new EADDRINUSE(address, 0);
    }
    if (!udpPorts.tryBind(port, this.#reuse)) {
      throw new EADDRINUSE(address, port);
    }
    const family = isIP(address) === 6 ? 'IPv6' : 'IPv4';
    this.#address = { address, family, port };
    udpPorts.setHandler(port, this.#handler);
    nextTick(() => {
      if (!this.#closed) this.emit('listening');
    });
  }

  bind(...args: unknown[]): this {
    this.#healthCheck();
    if (this.#address !== null) {
      throw new ERR_SOCKET_ALREADY_BOUND();
    }
    const last = args[args.length - 1];
    if (typeof last === 'function') {
      this.once('listening', last as () => void);
    }

    let [port, address] = args;
    if (port !== null && typeof port === 'object') {
      const options = port as BindOptions;
      address = options.address ?? '';
      port = options.port;
    } else if (typeof address === 'function') {
      address = '';
    }
    const portNumber = port == null ? 0 : validatePort(port, 'Port');
    let host: string;
    if (address === undefined || address === null || address === '') {
      host = this.type === 'udp6' ? DEFAULT_IPV6_ADDR : DEFAULT_IPV4_ADDR;
    } else {
      validateString(address, 'address');
      host = address;
    }

    try {
      this.#bind(portNumber, host, false);
    } catch (err) {
      nextTick(() => {
        this.emit('error', err);
      });
    }
    return this;
  }

  send(
    buffer: unknown,
    offset?: unknown,
    length?: unknown,
    port?: unknown,
    address?: unknown,
    callback?: unknown
  ): void {
    let list: Uint8Array[] | null;
    const connected = this.#remote !== null;
    if (!connected) {
      if (address || (port && typeof port !== 'function')) {
        buffer = sliceBuffer(buffer, offset, length);
      } else {
        callback = port;
        port = offset;
        address = length;
      }
    } else {
      if (typeof length === 'number') {
        buffer = sliceBuffer(buffer, offset, length);
        if (typeof port === 'function') {
          callback = port;
          port = null;
        }
      } else {
        callback = offset;
      }
      if (port || address) {
        throw new ERR_SOCKET_DGRAM_IS_CONNECTED();
      }
    }

    if (!ArrayBuffer.isView(buffer)) {
      if (typeof buffer === 'string') {
        list = [Buffer.from(buffer)];
      } else if (Array.isArray(buffer)) {
        list = fixBufferList(buffer);
        if (list === null) {
          throw new ERR_INVALID_ARG_TYPE(
            'buffer list arguments',
            ['Buffer', 'TypedArray', 'DataView', 'string'],
            buffer
          );
        }
      } else {
        throw new ERR_INVALID_ARG_TYPE(
          'buffer',
          ['Buffer', 'TypedArray', 'DataView', 'string'],
          buffer
        );
      }
    } else {
      list = [
        new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength),
      ];
    }

    let portNumber = 0;
    if (!connected) {
      portNumber = validatePort(port, 'Port', false);
    }

    if (typeof callback !== 'function') {
      callback = undefined;
    }
    if (typeof address === 'function') {
      callback = address;
      address = undefined;
    } else if (address != null) {
      validateString(address, 'address');
    }
    const cb = callback as SendCallback | undefined;

    this.#healthCheck();
    if (this.#address === null) {
      this.#bind(0, this.#loopback(), true);
    }

    if (list.length === 0) {
      list.push(Buffer.alloc(0));
    }
    const msg = toBuffer(list);

    let destAddress: string;
    let destPort: number;
    if (this.#remote !== null) {
      destAddress = this.#remote.address;
      destPort = this.#remote.port;
    } else {
      destAddress = address ? (address as string) : this.#loopback();
      destPort = portNumber;
    }
    if (destAddress === 'localhost') destAddress = this.#loopback();

    const flow = this.#flows.get(flowKey(destAddress, destPort));
    if (flow === undefined) {
      if (cb !== undefined) {
        nextTick(cb, new EHOSTUNREACH(destAddress, destPort));
      }
      return;
    }
    flow.writer
      .write(
        new Datagram(new Uint8Array(msg.buffer, msg.byteOffset, msg.length))
      )
      .then(
        () => {
          cb?.(null, msg.length);
        },
        (err: unknown) => {
          cb?.(err as Error);
        }
      );
  }

  sendto(
    buffer: unknown,
    offset: unknown,
    length: unknown,
    port: unknown,
    address: unknown,
    callback?: unknown
  ): void {
    validateNumber(offset, 'offset');
    validateNumber(length, 'length');
    validateNumber(port, 'port');
    validateString(address, 'address');
    this.send(buffer, offset, length, port, address, callback);
  }

  // Fixes the destination for subsequent send() calls; no datagrams are
  // exchanged, since UDP has no handshake.
  connect(port: unknown, address?: unknown, callback?: unknown): void {
    const portNumber = validatePort(port, 'Port', false);
    if (typeof address === 'function') {
      callback = address;
      address = undefined;
    } else if (address !== undefined) {
      validateString(address, 'address');
    }
    this.#healthCheck();
    if (this.#remote !== null) {
      throw new ERR_SOCKET_DGRAM_IS_CONNECTED();
    }
    if (typeof callback === 'function') {
      this.once('connect', callback as () => void);
    }
    if (this.#address === null) {
      this.#bind(0, this.#loopback(), true);
    }
    let host = address === undefined ? this.#loopback() : (address as string);
    if (host === 'localhost') host = this.#loopback();
    this.#remote = {
      address: host,
      family: isIP(host) === 6 ? 'IPv6' : 'IPv4',
      port: portNumber,
    };
    nextTick(() => {
      if (!this.#closed) this.emit('connect');
    });
  }

  disconnect(): void {
    this.#healthCheck();
    if (this.#remote === null) {
      throw new ERR_SOCKET_DGRAM_NOT_CONNECTED();
    }
    this.#remote = null;
  }

  close(callback?: () => void): this {
    if (typeof callback === 'function') {
      this.once('close', callback);
    }
    this.#healthCheck();
    this.#closed = true;
    if (this.#address !== null) {
      udpPorts.clearHandler(this.#address.port, this.#handler);
      udpPorts.release(this.#address.port);
    }
    // Ending each flow's read loop finishes its request (see #onFlow).
    for (const flow of this.#flows.values()) {
      flow.reader.cancel().catch(() => {});
    }
    this.#flows.clear();
    nextTick(() => {
      this.emit('close');
    });
    return this;
  }

  async [Symbol.asyncDispose](): Promise<void> {
    if (this.#closed) return;
    // eslint-disable-next-line @typescript-eslint/no-invalid-void-type
    const { promise, resolve } = Promise.withResolvers<void>();
    this.close(resolve);
    await promise;
  }

  address(): AddressInfo {
    this.#healthCheck();
    if (this.#address === null) {
      throw new EBADF('getsockname');
    }
    return { ...this.#address };
  }

  remoteAddress(): AddressInfo {
    this.#healthCheck();
    if (this.#remote === null) {
      throw new ERR_SOCKET_DGRAM_NOT_CONNECTED();
    }
    return { ...this.#remote };
  }

  setBroadcast(_flag: boolean): void {
    this.#healthCheck();
  }

  setTTL(ttl: unknown): number {
    validateNumber(ttl, 'ttl', 1, 255);
    this.#healthCheck();
    return ttl;
  }

  setMulticastTTL(ttl: unknown): number {
    validateNumber(ttl, 'ttl', 0, 255);
    this.#healthCheck();
    return ttl;
  }

  setMulticastLoopback(flag: boolean): boolean {
    this.#healthCheck();
    return flag;
  }

  setMulticastInterface(interfaceAddress: unknown): void {
    validateString(interfaceAddress, 'interfaceAddress');
    this.#healthCheck();
  }

  addMembership(multicastAddress: unknown, _interfaceAddress?: unknown): void {
    this.#healthCheck();
    if (!multicastAddress) {
      throw new ERR_MISSING_ARGS('multicastAddress');
    }
    throw new ERR_FEATURE_UNAVAILABLE_ON_PLATFORM('multicast');
  }

  dropMembership(multicastAddress: unknown, _interfaceAddress?: unknown): void {
    this.#healthCheck();
    if (!multicastAddress) {
      throw new ERR_MISSING_ARGS('multicastAddress');
    }
    throw new ERR_FEATURE_UNAVAILABLE_ON_PLATFORM('multicast');
  }

  addSourceSpecificMembership(
    sourceAddress: unknown,
    groupAddress: unknown,
    _interfaceAddress?: unknown
  ): void {
    this.#healthCheck();
    validateString(sourceAddress, 'sourceAddress');
    validateString(groupAddress, 'groupAddress');
    throw new ERR_FEATURE_UNAVAILABLE_ON_PLATFORM('multicast');
  }

  dropSourceSpecificMembership(
    sourceAddress: unknown,
    groupAddress: unknown,
    _interfaceAddress?: unknown
  ): void {
    this.#healthCheck();
    validateString(sourceAddress, 'sourceAddress');
    validateString(groupAddress, 'groupAddress');
    throw new ERR_FEATURE_UNAVAILABLE_ON_PLATFORM('multicast');
  }

  ref(): this {
    return this;
  }

  unref(): this {
    return this;
  }

  setRecvBufferSize(size: unknown): void {
    if (!Number.isInteger(size) || (size as number) < 0) {
      throw new ERR_SOCKET_BAD_BUFFER_SIZE();
    }
    this.#healthCheck();
    this.#recvBufferSize = size as number;
  }

  setSendBufferSize(size: unknown): void {
    if (!Number.isInteger(size) || (size as number) < 0) {
      throw new ERR_SOCKET_BAD_BUFFER_SIZE();
    }
    this.#healthCheck();
    this.#sendBufferSize = size as number;
  }

  getRecvBufferSize(): number {
    this.#healthCheck();
    return this.#recvBufferSize;
  }

  getSendBufferSize(): number {
    this.#healthCheck();
    return this.#sendBufferSize;
  }

  getSendQueueSize(): number {
    this.#healthCheck();
    return 0;
  }

  getSendQueueCount(): number {
    this.#healthCheck();
    return 0;
  }

  // The peer as this socket's family reports it: a v4-mapped peer on a
  // dual-stack listener is an IPv4 peer to a udp4 socket, and stays IPv6 to
  // a udp6 socket, as Linux reports it.
  #rinfo(address: string, port: number): Omit<RemoteInfo, 'size'> {
    if (this.type === 'udp4') {
      const v4 = stripMappedV4(address);
      if (v4 !== undefined) address = v4;
    }
    return {
      address,
      family: isIP(address) === 6 ? 'IPv6' : 'IPv4',
      port,
    };
  }

  // One platform flow: resolves when the peer's flow ends or this socket
  // closes, which is how long the inbound request lives.
  async #onFlow(socket: UdpInboundSocket): Promise<void> {
    const info = await socket.opened;
    if (this.#closed) {
      await socket.close();
      return;
    }
    const peer = parseAuthority(info.remoteAddress) ?? {
      address: GATEWAY_ADDRESS,
      port: udpPorts.ephemeral(),
    };
    const rinfo = this.#rinfo(peer.address, peer.port);
    const key = flowKey(rinfo.address, rinfo.port);
    const flow: Flow = {
      reader: socket.readable.getReader(),
      writer: socket.writable.getWriter(),
    };
    // A peer whose previous flow is still winding down is replaced.
    this.#flows.set(key, flow);
    try {
      for (;;) {
        const { value, done } = await flow.reader.read();
        if (done) break;
        const { data } = value;
        const msg = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
        this.emit('message', msg, { ...rinfo, size: msg.length });
      }
    } finally {
      if (this.#flows.get(key) === flow) this.#flows.delete(key);
      await flow.writer.close().catch(() => {});
    }
  }
}

export function createSocket(type: unknown, listener?: unknown): Socket {
  return new Socket(type, listener);
}
