// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing URL and URLSearchParams: parse failures and their messages,
// base handling, argument counts, `new`, and subclassing.

import { throws, strictEqual, ok } from 'node:assert';
import { usingTsImpl, urlStandard } from 'which-impl';

function invalid(message) {
  return { name: 'TypeError', message };
}

export const parseFailures = {
  test() {
    throws(() => new URL('nope'), invalid('Invalid URL string.'));
    // Each backend keeps its messages; the legacy one tells a bad base
    // from a bad relative URL.
    throws(
      () => new URL('/x', 'nope'),
      invalid(urlStandard ? 'Invalid URL string.' : 'Invalid base URL string.')
    );
    if (urlStandard) {
      throws(
        () => new URL('http://[', 'http://a/'),
        invalid('Invalid URL string.')
      );
    } else {
      // kj::Url resolves it as a relative reference.
      strictEqual(new URL('http://[', 'http://a/').href, 'http://[');
    }
  },
};

export const baseHandling = {
  test() {
    strictEqual(new URL('http://a/', undefined).href, 'http://a/');
    strictEqual(new URL('/x', new URL('http://b/y')).href, 'http://b/x');
    strictEqual(new URL('/x', 'http://b/y', 'ignored').href, 'http://b/x');
    // null is the string "null".
    throws(
      () => new URL('/x', null),
      invalid(urlStandard ? 'Invalid URL string.' : 'Invalid base URL string.')
    );
    strictEqual(new URL(null, 'http://a/').href, 'http://a/null');
    strictEqual(new URL(undefined, 'http://a/').href, 'http://a/undefined');
    throws(() => new URL(undefined), invalid('Invalid URL string.'));
  },
};

export const urlRequiresAnArgument = {
  test() {
    throws(
      () => new URL(),
      usingTsImpl
        ? { name: 'TypeError' }
        : invalid(
            "Failed to construct 'URL': constructor parameter 1 is not of type " +
              (urlStandard ? "'USVString'." : "'string'.")
          )
    );
  },
};

export const searchParamsArgumentIsOptional = {
  test() {
    strictEqual(new URLSearchParams().toString(), '');
    strictEqual(new URLSearchParams(undefined).toString(), '');
    strictEqual(new URLSearchParams(null).toString(), 'null=');
  },
};

export const constructorsRequireNew = {
  test() {
    // Ledger #3.
    for (const [ctor, name] of [
      [URL, 'URL'],
      [URLSearchParams, 'URLSearchParams'],
    ]) {
      throws(
        () => ctor('http://a/'),
        usingTsImpl
          ? { name: 'TypeError' }
          : invalid(
              `Failed to construct '${name}': Please use the 'new' operator, ` +
                'this object constructor cannot be called as a function.'
            )
      );
    }
  },
};

export const subclassing = {
  test() {
    class SubUrl extends URL {
      extra() {
        return this.pathname;
      }
    }
    const url = new SubUrl('http://a/p');
    ok(url instanceof URL);
    strictEqual(url.extra(), '/p');
    strictEqual(Object.getPrototypeOf(url), SubUrl.prototype);

    class SubParams extends URLSearchParams {
      count() {
        return this.size;
      }
    }
    const params = new SubParams('a=1&b=2');
    ok(params instanceof URLSearchParams);
    strictEqual(params.count(), 2);
    strictEqual(params.toString(), 'a=1&b=2');
  },
};

export const statics = {
  test() {
    if (!urlStandard && !usingTsImpl) return;
    strictEqual(URL.canParse('http://a'), true);
    strictEqual(URL.canParse('x'), false);
    strictEqual(URL.canParse('/x', 'http://a'), true);
    strictEqual(URL.canParse('/x', undefined), false);
    strictEqual(URL.canParse(undefined, 'http://a/'), true);
    // The backend's parser: the legacy one keeps 'http://a' as it is.
    const href = new URL('http://a').href;
    strictEqual(URL.parse('http://a').href, href);
    strictEqual(URL.parse('x'), null);
    strictEqual(URL.parse('http://a', undefined).href, href);
    ok(URL.parse('http://a') instanceof URL);
    throws(
      () => URL.canParse(),
      usingTsImpl
        ? { name: 'TypeError' }
        : invalid(
            "Failed to execute 'canParse' on 'URL': parameter 1 is not of type 'USVString'."
          )
    );
  },
};

export const objectUrlsAreUnsupported = {
  test() {
    if (!urlStandard && !usingTsImpl) return;
    throws(() => URL.createObjectURL(new Blob(['x'])), {
      name: 'Error',
      message: 'URL.createObjectURL() is not implemented',
    });
    // The argument is converted first.
    throws(
      () => URL.createObjectURL('x'),
      usingTsImpl
        ? { name: 'TypeError' }
        : invalid(
            "Failed to execute 'createObjectURL' on 'URL': parameter 1 is not of type 'File or Blob'."
          )
    );
    throws(() => URL.revokeObjectURL('blob:x'), {
      name: 'Error',
      message: 'URL.revokeObjectURL() is not implemented',
    });
  },
};
