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

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { default as cryptoImpl } from 'node-internal:crypto';
import { StringLike, Buffer } from 'node-internal:internal_buffer';

import { getArrayBufferOrView } from 'node-internal:crypto_util';

type ArrayLike = cryptoImpl.ArrayLike;

export function verifySpkac(spkac: StringLike | ArrayLike, encoding?: string) {
  return cryptoImpl.verifySpkac(
    getArrayBufferOrView(spkac as any, 'spkac', encoding)
  );
}

export function exportPublicKey(
  spkac: StringLike | ArrayLike,
  encoding?: string
) {
  const ret = cryptoImpl.exportPublicKey(
    getArrayBufferOrView(spkac as any, 'spkac', encoding)
  );
  return ret ? Buffer.from(ret) : Buffer.alloc(0);
}

export function exportChallenge(
  spkac: StringLike | ArrayLike,
  encoding?: string
) {
  const ret = cryptoImpl.exportChallenge(
    getArrayBufferOrView(spkac as any, 'spkac', encoding)
  );
  return ret ? Buffer.from(ret) : Buffer.alloc(0);
}

// The legacy implementation of this exposed the Certificate
// object and required that users create an instance before
// calling the member methods. This API pattern has been
// deprecated, however, as the method implementations do not
// rely on any object state.

// For backwards compatibility reasons, this cannot be converted into a
// ES6 Class.
export function Certificate(this: any) {
  if (!(this instanceof Certificate)) return new (Certificate as any)();
}

Certificate.prototype.verifySpkac = verifySpkac;
Certificate.prototype.exportPublicKey = exportPublicKey;
Certificate.prototype.exportChallenge = exportChallenge;

Certificate.exportChallenge = exportChallenge;
Certificate.exportPublicKey = exportPublicKey;
Certificate.verifySpkac = verifySpkac;
