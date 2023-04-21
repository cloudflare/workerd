// Type definitions for c++ implementation.

export class Socket {
  public readonly readable: any
  public readonly writable: any
  public readonly closed: Promise<void>
  public close(): Promise<void>
  public startTls(options: TlsOptions): Socket
}

export type TlsOptions = {
  expectedServerHostname?: string
}

export type SocketAddress = {
  hostname: string
  port: number
}

export type SocketOptions = {
  useSecureTransport: boolean
  allowHalfOpen: boolean
}

export function connect(address: string | SocketAddress, options?: SocketOptions): Socket;
