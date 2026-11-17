// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared by more than one behavior module.

import { urlPatternStandard } from 'which-impl';

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

// The component attributes, in the order the WebIDL declares them.
export const kComponents = [
  'protocol',
  'username',
  'password',
  'hostname',
  'port',
  'pathname',
  'search',
  'hash',
];

// URLPatternInit's members, as WebIDL orders them: lexicographically.
export const kInitMembers = [
  'baseURL',
  'hash',
  'hostname',
  'password',
  'pathname',
  'port',
  'protocol',
  'search',
  'username',
];

// The attributes: the components, then hasRegExpGroups, which only the
// standard parser has.
export const kAttributes = urlPatternStandard
  ? [...kComponents, 'hasRegExpGroups']
  : kComponents;

// An unpaired high surrogate, which a USVString conversion replaces with
// U+FFFD.
export const kLoneSurrogate = '\uD800';
