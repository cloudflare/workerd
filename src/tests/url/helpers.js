// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared by more than one behavior module.

export const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';

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

// The string-keyed own properties of `object`, in order, but for
// `constructor`, whose position JSG and a class put differently.
export function memberNames(object) {
  return Object.getOwnPropertyNames(object).filter((k) => k !== 'constructor');
}

// The URL attributes, in the order the WebIDL declares them.
export const kUrlAttributes = [
  'href',
  'origin',
  'protocol',
  'username',
  'password',
  'host',
  'hostname',
  'port',
  'pathname',
  'search',
  'searchParams',
  'hash',
];

export const kReadonlyUrlAttributes = ['origin', 'searchParams'];

// The URLSearchParams operations, in the order of the prototype.
export const kSearchParamsMethods = [
  'append',
  'delete',
  'get',
  'getAll',
  'has',
  'set',
  'sort',
  'entries',
  'keys',
  'values',
  'forEach',
  'toString',
];

// An unpaired high surrogate, which a USVString conversion replaces with
// U+FFFD.
export const kLoneSurrogate = '\uD800';
