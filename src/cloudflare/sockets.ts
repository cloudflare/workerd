// TODO: c++ built-ins do not yet support named exports
import sockets from 'cloudflare-internal:sockets';
export function connect(
  address: string | sockets.SocketAddress, options: sockets.SocketOptions
): sockets.Socket {
  return sockets.connect(address, options);
}
