import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';

export class ConnectProxy extends WorkerEntrypoint {
  async connect(socket) {
    // proxy for ConnectEndpoint instance on port 8083.
    let upstream = connect('localhost:8083');
    await socket.proxyTo(upstream);
  }
}

export class ConnectEndpoint extends WorkerEntrypoint {
  async connect(socket) {
    const enc = new TextEncoder();
    let writer = socket.writable.getWriter();
    await writer.write(enc.encode('hello-from-endpoint'));
    await writer.close();
  }
}
