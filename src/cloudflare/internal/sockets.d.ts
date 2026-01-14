// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for c++ implementation.

import { type ServiceStub } from 'cloudflare-internal:workers'

export class Socket {
  readonly readable: unknown
  readonly writable: unknown
  readonly closed: Promise<void>
  close(): Promise<void>
  startTls(options: TlsOptions): Socket
}

export type TlsOptions = {
  expectedServerHostname?: string
}

export type SocketAddress = {
  hostname: string
  port: number
}

export type SocketOptions = {
  secureTransport?: 'off' | 'on' | 'starttls'
  allowHalfOpen?: boolean
}

export function connect(
  address: string | SocketAddress,
  options?: SocketOptions,
): Socket

export function internalNewHttpClient(socket: Socket): Promise<ServiceStub>
