// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared by more than one behavior module.

// A property descriptor's attributes as a short string: its kind ('data'
// or 'accessor'), then w, e and c for writable, enumerable and
// configurable.
export function attributes(descriptor) {
  return [
    'value' in descriptor ? 'data' : 'accessor',
    descriptor.writable ? 'w' : '',
    descriptor.enumerable ? 'e' : '',
    descriptor.configurable ? 'c' : '',
  ].join('');
}
