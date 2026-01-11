// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Type definitions for c++ implementation.

import { type ServiceStub } from 'cloudflare-internal:workers';

export class Socket {
  readonly readable: unknown;
  readonly writable: unknown;
  readonly closed: Promise<void>;
  close(): Promise<void>;
  startTls(options: TlsOptions): Socket;
}

export type TlsOptions = {
  expectedServerHostname?: string;
};

export type SocketAddress = {
  hostname: string;
  port: number;
};

export type SocketOptions = {
  secureTransport?: 'off' | 'on' | 'starttls';
  allowHalfOpen?: boolean;
  /**
   * Optional PEM-encoded CA certificate(s) to use for verifying the server certificate
   * during TLS handshake. If set, these certificates will be used instead of the default
   * system CA store. This is useful for connecting through intercepting proxies that use
   * custom CA certificates.
   *
   * NOTE: This option only works when deployed to Cloudflare Workers. For local development
   * with workerd, configure trusted certificates in your workerd.capnp config file instead:
   *
   *   ( name = "internet",
   *     network = (
   *       allow = ["public"],
   *       tlsOptions = (trustedCertificates = [embed "my-ca.pem"])
   *     )
   *   )
   */
  caCerts?: ArrayBuffer;
};

export function connect(
  address: string | SocketAddress,
  options?: SocketOptions
): Socket;

export function internalNewHttpClient(socket: Socket): Promise<ServiceStub>;
