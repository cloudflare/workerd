import { Server as HttpServer } from 'node-internal:internal_http_server';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import type { Server as _Server } from 'node:https';

export class Server extends HttpServer implements _Server {
  addContext(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('addContext');
  }

  getTicketKeys(): Buffer<ArrayBufferLike> {
    throw new ERR_METHOD_NOT_IMPLEMENTED('getTicketKeys');
  }

  setSecureContext(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('setSecureContext');
  }

  setTicketKeys(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('setTicketKeys');
  }
}
