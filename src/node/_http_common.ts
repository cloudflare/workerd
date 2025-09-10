// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  _checkIsHttpToken,
  _checkInvalidHeaderChar,
  chunkExpression,
} from 'node-internal:internal_http';
import { METHODS } from 'node-internal:internal_http_constants';
import { kIncomingMessage } from 'node-internal:internal_http_util';
export {
  _checkIsHttpToken,
  _checkInvalidHeaderChar,
  chunkExpression,
  kIncomingMessage,
};
export const continueExpression = /(?:^|\W)100-continue(?:$|\W)/i;
export const methods = METHODS;

export default {
  _checkIsHttpToken,
  _checkInvalidHeaderChar,
  chunkExpression,
  continueExpression,
  methods,
  kIncomingMessage,
};
