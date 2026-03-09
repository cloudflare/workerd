// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Server as HttpServer } from 'node-internal:internal_http_server';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import type { Server as _Server } from 'node:https';

export class Server extends HttpServer implements _Server {
  addContext(): void {
    // Workerd don't support TLS SecureContext. It doesn't
    // make sense to support this for us.
    throw new ERR_METHOD_NOT_IMPLEMENTED('addContext');
  }

  getTicketKeys(): Buffer<ArrayBuffer> {
    // We don't support TLS ticket keys.
    throw new ERR_METHOD_NOT_IMPLEMENTED('getTicketKeys');
  }

  setSecureContext(): void {
    // We don't support this for now.
    // TODO(soon): Investigate if we want to have a implementation
    // that just makes input validations and ignores the context.
    throw new ERR_METHOD_NOT_IMPLEMENTED('setSecureContext');
  }

  setTicketKeys(): void {
    // We don't support TLS ticket keys.
    throw new ERR_METHOD_NOT_IMPLEMENTED('setTicketKeys');
  }
}
