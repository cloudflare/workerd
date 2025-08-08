// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import {
  checkServerIdentity,
  convertALPNProtocols,
  createServer,
  Server,
  getCiphers,
} from 'node-internal:internal_tls';
import {
  createSecureContext,
  SecureContext,
} from 'node-internal:internal_tls_common';
import * as constants from 'node-internal:internal_tls_constants';
import { TLSSocket, connect } from 'node-internal:internal_tls_wrap';
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

let createSecurePair = undefined;

if (!Cloudflare.compatibilityFlags.remove_nodejs_compat_eol) {
  createSecurePair = function createSecurePair(): void {
    throw new ERR_METHOD_NOT_IMPLEMENTED('createSecurePair');
  };
}

export * from 'node-internal:internal_tls_constants';
export {
  TLSSocket,
  connect,
  createSecureContext,
  createServer,
  checkServerIdentity,
  SecureContext,
  Server,
  convertALPNProtocols,
  getCiphers,
  createSecurePair,
};
export default {
  SecureContext,
  Server,
  TLSSocket,
  connect,
  createSecureContext,
  createServer,
  checkServerIdentity,
  convertALPNProtocols,
  getCiphers,
  createSecurePair,
  ...constants,
};
