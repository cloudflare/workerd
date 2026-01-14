import type { Server as _Server } from 'node:https'
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors'
import { Server as HttpServer } from 'node-internal:internal_http_server'

export class Server extends HttpServer implements _Server {
  addContext(): void {
    // Workerd don't support TLS SecureContext. It doesn't
    // make sense to support this for us.
    throw new ERR_METHOD_NOT_IMPLEMENTED('addContext')
  }

  getTicketKeys(): Buffer {
    // We don't support TLS ticket keys.
    throw new ERR_METHOD_NOT_IMPLEMENTED('getTicketKeys')
  }

  setSecureContext(): void {
    // We don't support this for now.
    // TODO(soon): Investigate if we want to have a implementation
    // that just makes input validations and ignores the context.
    throw new ERR_METHOD_NOT_IMPLEMENTED('setSecureContext')
  }

  setTicketKeys(): void {
    // We don't support TLS ticket keys.
    throw new ERR_METHOD_NOT_IMPLEMENTED('setTicketKeys')
  }
}
