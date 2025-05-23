// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  validateHeaderName,
  validateHeaderValue,
} from 'node-internal:internal_http';
import { METHODS, STATUS_CODES } from 'node-internal:internal_http_constants';

export { validateHeaderName, validateHeaderValue, METHODS, STATUS_CODES };
export default {
  validateHeaderName,
  validateHeaderValue,
  METHODS,
  STATUS_CODES,
};
