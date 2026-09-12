declare module 'cloudflare:node' {
  interface NodeStyleServer {
    listen(...args: unknown[]): this;
    address(): { port?: number | null | undefined } | null;
  }

  export function httpServerHandler(port: number): ExportedHandler;
  export function httpServerHandler(options: { port: number }): ExportedHandler;
  export function httpServerHandler(server: NodeStyleServer): ExportedHandler;

  /**
   * Dispatches a request to the `http.Server` listening on the given port and
   * resolves with its response. The direct form of `httpServerHandler()`.
   */
  export function handleAsNodeRequest(
    port: number | { port: number },
    request: Request,
    env?: unknown,
    ctx?: ExecutionContext
  ): Promise<Response>;

  /**
   * Routes inbound sockets to the `net.Server` listening on the port each
   * socket arrived on.
   */
  export function connectHandler(): ExportedHandler;

  /**
   * Dispatches an inbound socket to the `net.Server` listening on the port it
   * arrived on, resolving when the connection has closed. The direct form of
   * `connectHandler()`, for use inside a `connect()` handler, such as a
   * Durable Object's.
   */
  export function handleAsNodeConnection(
    socket: Socket,
    env?: unknown,
    ctx?: ExecutionContext
  ): Promise<void>;
}
