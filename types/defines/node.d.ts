declare module 'cloudflare:node' {
  interface NodeStyleServer {
    listen(...args: unknown[]): this;
    address(): { port?: number | null | undefined };
  }

  export function httpServerHandler(port: number): ExportedHandler;
  export function httpServerHandler(options: { port: number }): ExportedHandler;
  export function httpServerHandler(server: NodeStyleServer): ExportedHandler;
}
