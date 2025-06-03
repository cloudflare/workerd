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
import { getBindingPath } from './common';

// @ts-expect-error We're just exposing enough stuff for the tests to pass; it's not a perfect match
globalThis.self = globalThis;

// eslint-disable-next-line @typescript-eslint/no-unsafe-assignment,@typescript-eslint/no-unsafe-member-access --  We're just exposing enough stuff for the tests to pass; it's not a perfect match
globalThis.Window = Object.getPrototypeOf(globalThis).constructor;

const realFetch = globalThis.fetch;
const realRequest = globalThis.Request;

function relativizeUrl(input: URL | string): string {
  return new URL(input, globalThis.state.testUrl).href;
}

function relativizeRequest(
  input: RequestInfo | URL,
  init?: RequestInit
): Request {
  if (input instanceof Request) {
    return new realRequest(
      relativizeRequest(input.url),
      new realRequest(input, init)
    );
  } else if (input instanceof URL) {
    return new realRequest(relativizeUrl(input), init);
  } else {
    return new realRequest(relativizeUrl(input), init);
  }
}

globalThis.Request = class _Request extends Request {
  public constructor(input: RequestInfo | URL, init?: RequestInit) {
    super(relativizeRequest(input, init));
  }
};

globalThis.Response = class _Response extends Response {
  public static override redirect(
    url: string | URL,
    status?: number
  ): Response {
    return super.redirect(relativizeUrl(url), status);
  }
};

globalThis.fetch = async (
  input: RequestInfo | URL,
  init?: RequestInit
): Promise<Response> => {
  if (typeof input === 'string' && input.endsWith('.json')) {
    const relativePath = getBindingPath(
      path.dirname(globalThis.state.testFileName),
      input
    );
    // WPT sometimes uses fetch to load a resource file, we "serve" this from the bindings
    const exports: unknown = globalThis.state.env[relativePath];
    if (exports === undefined) {
      throw new Error(`Unable to load resources file ${input} from bindings`);
    }

    return new Response(JSON.stringify(exports));
  }
  return realFetch(relativizeRequest(input, init));
};

class _Location {
  public get ancestorOrigins(): DOMStringList {
    return {
      length: 0,
      item(_index: number): string | null {
        return null;
      },
      contains(_string: string): boolean {
        return false;
      },
    };
  }

  public get hash(): string {
    return globalThis.state.testUrl.hash;
  }

  public get host(): string {
    return globalThis.state.testUrl.host;
  }

  public get hostname(): string {
    return globalThis.state.testUrl.hostname;
  }

  public get href(): string {
    return globalThis.state.testUrl.href;
  }

  public get origin(): string {
    return globalThis.state.testUrl.origin;
  }

  public get pathname(): string {
    return globalThis.state.testUrl.pathname;
  }

  public get port(): string {
    return globalThis.state.testUrl.port;
  }

  public get protocol(): string {
    return globalThis.state.testUrl.protocol;
  }

  public get search(): string {
    return globalThis.state.testUrl.search;
  }

  public assign(url: string): void {
    globalThis.state.testUrl = new URL(url);
  }

  public reload(): void {}

  public replace(url: string): void {
    globalThis.state.testUrl = new URL(url);
  }

  public toString(): string {
    return globalThis.state.testUrl.href;
  }
}

globalThis.location = new _Location();
