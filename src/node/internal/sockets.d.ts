export interface SocketInfo {
  remoteAddress?: string | null;
  localAddress?: string | null;
}

export type Reader = ReadableStreamBYOBReader & {
  getReader(opts: { mode: 'byob' }): ReadableStreamBYOBReader;
};

export interface Writer extends WritableStream {
  close(): Promise<void>;
  write(data: string | ArrayBufferView): Promise<void>;
  closed: Promise<void>;
  desiredSize: number | null;
  releaseLock(): void;
}

export interface Socket {
  opened: Promise<SocketInfo>;
  closed: Promise<void>;
  close(): Promise<void>;
  readable: Reader;
  writable: Writer;
  startTls(): Socket;

  readonly upgraded: boolean;
  readonly secureTransport: 'on' | 'off' | 'starttls';
}

declare namespace sockets {
  function connect(
    input: string,
    options: {
      allowHalfOpen?: boolean | undefined;
      highWatermark?: number | undefined;
      secureTransport: 'on' | 'off' | 'starttls';
    }
  ): Socket;
}
export default sockets;
