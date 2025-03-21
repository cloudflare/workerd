import type { Buffer } from 'node-internal:internal_buffer';

export interface SocketInfo {
  remoteAddress?: string | null;
  localAddress?: string | null;
}

declare namespace sockets {
  function connect(
    input: string,
    options: {
      allowHalfOpen?: boolean | undefined;
      highWatermark?: number | undefined;
      secureTransport: 'on' | 'off' | 'starttls';
    }
  ): {
    opened: Promise<SocketInfo>;
    closed: Promise<void>;
    close(): Promise<void>;
    readable: {
      getReader(options: Record<string, string>): {
        close(): Promise<void>;
        read(value: unknown): Promise<{ value: Buffer; done: boolean }>;
      };
    };
    writable: {
      getWriter(): {
        close(): Promise<void>;
        write(data: string | ArrayBufferView): Promise<void>;
      };
    };
  };
}
export default sockets;
