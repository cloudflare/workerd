// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  _checkIsHttpToken,
  _checkInvalidHeaderChar,
} from 'node-internal:internal_http';

export { _checkIsHttpToken, _checkInvalidHeaderChar };
export const chunkExpression = /(?:^|\W)chunked(?:$|\W)/i;
export const continueExpression = /(?:^|\W)100-continue(?:$|\W)/i;

export default {
  _checkIsHttpToken,
  _checkInvalidHeaderChar,
  chunkExpression,
  continueExpression,
};
