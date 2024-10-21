// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  escape,
  parse,
  stringify,
  unescape,
  unescapeBuffer,
} from 'node-internal:internal_querystring';

export const encode = stringify;
export const decode = parse;

export { escape, parse, stringify, unescape, unescapeBuffer };

export default {
  decode,
  encode,
  escape,
  parse,
  stringify,
  unescape,
  unescapeBuffer,
};
