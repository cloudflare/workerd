// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
export function domainToASCII(domain: string): string;
export function domainToUnicode(domain: string): string;
export function format(
  href: string,
  hash: boolean,
  unicode: boolean,
  search: boolean,
  auth: boolean
): string;
export function toASCII(input: string): string;
export function canonicalizeIp(input: string): string;
