// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The constructor: argument conversion, its errors and their order, and
// the state of a new EventSource. Divergence #3 (see AGENTS.md): calling
// the TypeScript constructor without `new` throws the language's
// TypeError.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const kUrl = 'http://example.org/request-headers';

function invalidUrl(url) {
  return {
    name: 'SyntaxError',
    message: `Cannot open an EventSource to '${url}'. The URL is invalid.`,
  };
}

const kWithCredentials = {
  name: 'NotSupportedError',
  message:
    'The init.withCredentials option is not supported. It must be false ' +
    'or undefined.',
};

// Evaluated outside of any request.
let moduleScopeErrors = [];
for (const create of [
  () => new EventSource(kUrl),
  () => EventSource.from(new ReadableStream()),
]) {
  try {
    create();
    moduleScopeErrors.push('created');
  } catch (e) {
    moduleScopeErrors.push(`${e.name}: ${e.message}`);
  }
}

export const constructorRequiresNew = {
  test() {
    throws(() => EventSource(kUrl), {
      name: 'TypeError',
      message: usingTsImpl
        ? "Class constructor EventSource cannot be invoked without 'new'"
        : "Failed to construct 'EventSource': Please use the 'new' " +
          'operator, this object constructor cannot be called as a function.',
    });
  },
};

export const urlIsConvertedToAString = {
  test(ctrl, env) {
    throws(() => new EventSource(), {
      name: 'TypeError',
      message:
        "Failed to construct 'EventSource': constructor parameter 1 is not " +
        "of type 'string'.",
    });
    throws(() => new EventSource(Symbol()), {
      name: 'TypeError',
      message: 'Cannot convert a Symbol value to a string',
    });
    throws(() => new EventSource(123), invalidUrl('123'));
    const source = new EventSource(
      { toString: () => kUrl },
      { fetcher: env.subrequest }
    );
    strictEqual(source.url, kUrl);
    source.close();
  },
};

export const urlMustBeAbsolute = {
  test() {
    for (const url of ['', '/relative', 'not a valid url']) {
      throws(() => new EventSource(url), invalidUrl(url));
    }
  },
};

export const initMustBeAnObject = {
  test(ctrl, env) {
    for (const init of [5, 'x', true]) {
      throws(() => new EventSource(kUrl, init), {
        name: 'TypeError',
        message:
          "Failed to construct 'EventSource': constructor parameter 2 is " +
          "not of type 'EventSourceInit'.",
      });
    }
    // null and undefined are an empty init, and a function is an object.
    for (const init of [null, undefined, () => {}]) {
      new EventSource(kUrl, init).close();
    }
    new EventSource(kUrl, { fetcher: env.subrequest }).close();
  },
};

export const withCredentialsMustBeFalsy = {
  test(ctrl, env) {
    for (const withCredentials of [true, 1, 'x', {}]) {
      throws(
        () =>
          new EventSource(kUrl, { withCredentials, fetcher: env.subrequest }),
        kWithCredentials
      );
    }
    for (const withCredentials of [false, undefined, null, 0, '']) {
      new EventSource(kUrl, {
        withCredentials,
        fetcher: env.subrequest,
      }).close();
    }
  },
};

export const fetcherMustBeAFetcher = {
  test() {
    for (const fetcher of [{}, null, 5, 'x']) {
      throws(() => new EventSource(kUrl, { fetcher }), {
        name: 'TypeError',
        message:
          "Incorrect type for the 'fetcher' field on 'EventSourceInit': the " +
          "provided value is not of type 'Fetcher'.",
      });
    }
  },
};

// Both arguments are converted, init's members in order, before anything is
// checked.
export const conversionPrecedesChecks = {
  test() {
    const reads = [];
    throws(
      () =>
        new EventSource(
          {
            toString() {
              reads.push('url');
              return 'not valid';
            },
          },
          {
            get withCredentials() {
              reads.push('withCredentials');
              return true;
            },
            get fetcher() {
              reads.push('fetcher');
              return undefined;
            },
          }
        ),
      kWithCredentials
    );
    deepStrictEqual(reads, ['url', 'withCredentials', 'fetcher']);
  },
};

export const newEventSourceState = {
  test(ctrl, env) {
    const source = new EventSource('HTTP://Example.org:80/a/../b?q#frag', {
      fetcher: env.subrequest,
    });
    strictEqual(source.url, 'http://example.org/b?q#frag');
    strictEqual(source.readyState, EventSource.CONNECTING);
    strictEqual(source.withCredentials, false);
    source.close();
  },
};

export const onlyWithinARequest = {
  test() {
    const error =
      'NotSupportedError: An EventSource can only be created within the ' +
      'context of a worker request.';
    deepStrictEqual(moduleScopeErrors, [error, error]);
  },
};
