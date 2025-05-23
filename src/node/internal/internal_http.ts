// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright Joyent and Node contributors. All rights reserved. MIT license.

import {
  ERR_INVALID_HTTP_TOKEN,
  ERR_INVALID_CHAR,
  ERR_HTTP_INVALID_HEADER_VALUE,
} from 'node-internal:internal_errors';

const tokenRegExp = /^[\^_`a-zA-Z\-0-9!#$%&'*+.|~]+$/;
/**
 * Verifies that the given val is a valid HTTP token
 * per the rules defined in RFC 7230
 * See https://tools.ietf.org/html/rfc7230#section-3.2.6
 */
export function _checkIsHttpToken(val: string): boolean {
  return tokenRegExp.test(val);
}

const headerCharRegex = /[^\t\x20-\x7e\x80-\xff]/;
/**
 * True if val contains an invalid field-vchar
 *  field-value    = *( field-content / obs-fold )
 *  field-content  = field-vchar [ 1*( SP / HTAB ) field-vchar ]
 *  field-vchar    = VCHAR / obs-text
 */
export function _checkInvalidHeaderChar(val: string): boolean {
  return headerCharRegex.test(val);
}

export function validateHeaderName(
  name: string,
  label: string = 'Header name'
): void {
  if (typeof name !== 'string' || !name || !_checkIsHttpToken(name)) {
    throw new ERR_INVALID_HTTP_TOKEN(label, name);
  }
}

export function validateHeaderValue(
  name: string,
  value: string | undefined
): void {
  if (value === undefined) {
    throw new ERR_HTTP_INVALID_HEADER_VALUE(value, name);
  }
  if (_checkInvalidHeaderChar(value)) {
    throw new ERR_INVALID_CHAR('header content', name);
  }
}
