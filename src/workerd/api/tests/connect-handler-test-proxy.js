import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class ConnectProxy extends WorkerEntrypoint {
  async connect({ socket }) {
    // proxy for ConnectEndpoint instance on port 8083.
    let upstream = connect('localhost:8083');
    await Promise.all([
      socket.readable.pipeTo(upstream.writable),
      upstream.readable.pipeTo(socket.writable),
    ]);
  }
}

export class ConnectEndpoint extends WorkerEntrypoint {
  connect({ socket }) {
    const enc = new TextEncoder();
    socket.writable.getWriter().write(enc.encode('hello-from-endpoint'));
  }
}
