// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Copyright © web-platform-tests contributors. BSD license
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
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

import path from 'node:path';

export type UnknownFunc = (...args: unknown[]) => unknown;
export type TestFn = UnknownFunc;
export type PromiseTestFn = () => Promise<unknown>;

export class FilterList {
  // Matches any input
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private matchesAll: boolean = false;

  // List of strings to match exactly
  // eslint-disable-next-line no-restricted-syntax
  private strings: Set<string> = new Set();

  // List of regexps to match against
  // eslint-disable-next-line no-restricted-syntax
  private regexps: RegExp[] = [];

  // Regexes which never matched any of the inputs
  // We keep this set so we can warn the user about this.
  // eslint-disable-next-line no-restricted-syntax
  private unmatchedRegexps: Set<RegExp> = new Set();

  constructor(filters: (string | RegExp)[] | true | undefined) {
    if (filters === undefined) {
      return;
    }

    if (filters === true) {
      this.matchesAll = true;
      return;
    }

    for (const filter of filters) {
      if (typeof filter === 'string') {
        this.strings.add(filter);
      } else {
        this.regexps.push(filter);
      }
    }

    this.unmatchedRegexps = new Set(this.regexps);
  }

  has(input: string): boolean {
    if (this.matchesAll) {
      return true;
    }

    if (this.strings.has(input)) {
      return true;
    }

    return this.regexps.some((r) => r.test(input));
  }

  delete(input: string): boolean {
    if (this.matchesAll) {
      return true;
    }

    if (this.strings.delete(input)) {
      return true;
    }

    const maybeMatch = this.regexps.find((r) => r.test(input));

    if (maybeMatch !== undefined) {
      this.unmatchedRegexps.delete(maybeMatch);
      return true;
    }

    return false;
  }

  getUnmatched(): Set<string | RegExp> {
    return this.strings.union(this.unmatchedRegexps);
  }
}

export function sanitize_unpaired_surrogates(str: string): string {
  // Test logs will be exported to XML, so we must escape any characters that
  // are forbidden in an XML CDATA section, namely "[...] the surrogate blocks,
  // FFFE, and FFFF".
  // See https://www.w3.org/TR/REC-xml/#NT-Char

  return str
    .replace(
      /([\ud800-\udbff]+)(?![\udc00-\udfff])|(^|[^\ud800-\udbff])([\udc00-\udfff]+)/g,
      function (_, low?: string, prefix?: string, high?: string) {
        let output = prefix || ''; // prefix may be undefined
        const string: string = low || high || ''; // only one of these alternates can match

        for (const ch of string) {
          output += code_unit_str(ch);
        }
        return output;
      }
    )
    .replace(/(\uffff|\ufffe)/g, function (_, invalid_chars?: string) {
      let output = '';

      for (const ch of invalid_chars || '') {
        output += code_unit_str(ch);
      }
      return output;
    });
}

function code_unit_str(char: string): string {
  return 'U+' + char.charCodeAt(0).toString(16);
}

export type HostInfo = {
  REMOTE_HOST: string;
  HTTP_ORIGIN: string;
  HTTP_REMOTE_ORIGIN: string;
  HTTPS_ORIGIN: string;
  ORIGIN: string;
  HTTPS_REMOTE_ORIGIN: string;
  HTTP_PORT: string;
  HTTPS_PORT: string;
};

export function getHostInfo(): HostInfo {
  const httpUrl = globalThis.state.testUrl;

  const httpsUrl = new URL(httpUrl);
  httpsUrl.protocol = 'https';

  // If the environment variable HTTPS_PORT is set, the wpt server is running as a sidecar.
  // Update the URL's port so we can connect to it
  httpsUrl.port = globalThis.state.env.HTTPS_PORT ?? '';

  return {
    REMOTE_HOST: httpUrl.hostname,
    HTTP_ORIGIN: httpUrl.origin,
    ORIGIN: httpUrl.origin,
    HTTP_REMOTE_ORIGIN: httpUrl.origin,
    HTTPS_ORIGIN: httpsUrl.origin,
    HTTPS_REMOTE_ORIGIN: httpsUrl.origin,
    HTTP_PORT: httpUrl.port,
    HTTPS_PORT: httpsUrl.port,
  };
}

export function getBindingPath(base: string, rawPath: string): string {
  if (path.isAbsolute(rawPath)) {
    return rawPath;
  }

  return path.relative('/', path.resolve(base, rawPath));
}
