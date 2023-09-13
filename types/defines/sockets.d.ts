declare module "cloudflare:sockets" {
  function _connect(address: string | SocketAddress, options?: SocketOptions): Socket;
  export { _connect as connect };
}
