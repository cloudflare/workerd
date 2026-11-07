// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The constructor: its overloads, its defaults, the patterns each parser
// makes, and the errors each throws. The standard parser is ada, the legacy
// one jsg::UrlPattern; their results, quirks included, are pinned as they
// are.

import { throws, strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl, urlPatternStandard } from 'which-impl';
import { kComponents } from 'helpers';

function components(pattern) {
  return kComponents.map((name) => pattern[name]);
}

// The standard parser's one message for every failure.
const kFailed = {
  name: 'TypeError',
  message: 'Failed to construct URLPattern',
};

function legacyError(message) {
  return { name: 'TypeError', message };
}

export const constructorRequiresNew = {
  test() {
    throws(
      () => URLPattern({}),
      // Ledger #9.
      usingTsImpl
        ? { name: 'TypeError' }
        : {
            name: 'TypeError',
            message:
              "Failed to construct 'URLPattern': Please use the 'new' operator, this object constructor cannot be called as a function.",
          }
    );
  },
};

export const defaults = {
  test() {
    const wildcards = Array(8).fill('*');
    for (const make of [
      () => new URLPattern(),
      () => new URLPattern(undefined),
      () => new URLPattern(undefined, undefined),
      () => new URLPattern(null),
      () => new URLPattern({}),
      () => new URLPattern({}, {}),
      () => new URLPattern({}, null),
    ]) {
      deepStrictEqual(components(make()), wildcards);
    }
  },
};

export const patternStrings = {
  test() {
    // A string pattern leaves the components after the last one it names
    // as wildcards in the standard parser, empty in the legacy one.
    const later = urlPatternStandard ? '*' : '';
    deepStrictEqual(components(new URLPattern('https://example.com/books')), [
      'https',
      later,
      later,
      'example.com',
      '',
      '/books',
      later,
      later,
    ]);
    // Against a baseURL, the standard parser leaves the credentials
    // wildcards too.
    const credentials = urlPatternStandard ? '*' : '';
    deepStrictEqual(
      components(new URLPattern('/books/:id', 'https://example.com')),
      [
        'https',
        credentials,
        credentials,
        'example.com',
        '',
        '/books/:id',
        later,
        later,
      ]
    );
    // In a pattern string ':' starts a named group and '?' makes the part
    // before it optional, so 'u:p' is a username and '*?' an optional
    // wildcard in the pathname.
    deepStrictEqual(
      components(
        new URLPattern('https://u:p@*.example.com:8080/a/*?q=:v#frag')
      ),
      [
        'https',
        'u:p',
        credentials,
        '*.example.com',
        '8080',
        '/a/*?q=:v',
        '',
        'frag',
      ]
    );
  },
};

export const initPatterns = {
  test() {
    deepStrictEqual(
      components(new URLPattern({ protocol: 'http:', pathname: '/a/:b' })),
      ['http', '*', '*', '*', '*', '/a/:b', '*', '*']
    );
    // The standard parser removes a scheme-default port; the legacy one
    // does not.
    strictEqual(
      new URLPattern({ protocol: 'http', port: '80' }).port,
      urlPatternStandard ? '' : '80'
    );
    // A baseURL fills in the components before the first one given: the
    // standard parser leaves the credentials wildcards, and the legacy one
    // takes the later components too.
    deepStrictEqual(
      components(
        new URLPattern({ pathname: '/x', baseURL: 'https://u@a.com/p/q?s#h' })
      ),
      urlPatternStandard
        ? ['https', '*', '*', 'a.com', '', '/x', '*', '*']
        : ['https', 'u', '', 'a.com', '', '/x', 's', 'h']
    );
  },
};

export const overloads = {
  test() {
    // (input, baseURL)
    strictEqual(new URLPattern('/a', 'https://b/').hostname, 'b');
    // (input, options): an object, null or undefined second argument.
    strictEqual(
      new URLPattern({ pathname: '/a' }, { ignoreCase: true }).test({
        pathname: '/A',
      }),
      true
    );
    // (input, baseURL, options)
    strictEqual(
      new URLPattern('/a', 'https://b/', { ignoreCase: true }).test(
        'https://b/A'
      ),
      true
    );
    // A second argument that is not an object is a baseURL.
    throws(
      () => new URLPattern('/a', 5),
      urlPatternStandard ? kFailed : legacyError('Invalid base URL.')
    );
    // The legacy parser ignores a baseURL given with an init.
    if (urlPatternStandard) {
      throws(() => new URLPattern({}, 'https://a/'), kFailed);
    } else {
      strictEqual(new URLPattern({}, 'https://a/').hostname, '*');
    }
  },
};

export const threeArgumentOverload = {
  test() {
    // With three arguments the second is the baseURL, whatever its type
    // (ledger #7); the C++ makes undefined no baseURL and an object the
    // options.
    const make = () =>
      new URLPattern('https://a/x', undefined, { ignoreCase: true });
    if (usingTsImpl) {
      throws(
        make,
        urlPatternStandard ? kFailed : legacyError('Invalid base URL.')
      );
    } else {
      strictEqual(make().test('https://a/X'), true);
    }

    const log = [];
    const base = {
      toString() {
        log.push('base');
        return 'https://a/';
      },
      get ignoreCase() {
        log.push('ignoreCase');
        return true;
      },
    };
    const make3 = () =>
      new URLPattern('/x', base, {
        get ignoreCase() {
          log.push('options');
          return false;
        },
      });
    if (usingTsImpl) {
      strictEqual(make3().test('https://a/X'), false);
      deepStrictEqual(log, ['base', 'options']);
    } else {
      // The object is the options, so the relative pattern has no baseURL.
      throws(
        make3,
        urlPatternStandard
          ? kFailed
          : legacyError(
              'Syntax error in URL Pattern: a relative pattern must have a base URL.'
            )
      );
      // The third argument is read too, and ignored.
      deepStrictEqual(log, ['ignoreCase', 'options']);
    }
  },
};

export const optionsArgument = {
  test() {
    for (const truthy of ['yes', 1, {}]) {
      strictEqual(
        new URLPattern({ pathname: '/a' }, { ignoreCase: truthy }).test({
          pathname: '/A',
        }),
        true
      );
    }
    strictEqual(
      new URLPattern({ pathname: '/a' }, { ignoreCase: 0 }).test({
        pathname: '/A',
      }),
      false
    );
    // The patterns are the same with ignoreCase.
    strictEqual(
      new URLPattern({ pathname: '/A' }, { ignoreCase: true }).pathname,
      '/A'
    );
    throws(
      () => new URLPattern('https://a/x', 'https://a', 'x'),
      usingTsImpl
        ? { name: 'TypeError' }
        : {
            name: 'TypeError',
            message:
              "Failed to construct 'URLPattern': constructor parameter 3 is not of type 'URLPatternOptions'.",
          }
    );
    strictEqual(
      new URLPattern('https://a/x', 'https://a', null).pathname,
      '/x'
    );
  },
};

export const nonObjectInputsAreStrings = {
  test() {
    // 5 and true are pattern strings: relative, with no baseURL.
    const relative = urlPatternStandard
      ? kFailed
      : legacyError(
          'Syntax error in URL Pattern: a relative pattern must have a base URL.'
        );
    throws(() => new URLPattern(5), relative);
    throws(() => new URLPattern(true), relative);
    strictEqual(new URLPattern(5, 'https://a/').pathname, '/5');
    throws(() => new URLPattern(Symbol()), { name: 'TypeError' });
    // A function is an object, so an init.
    const init = () => {};
    init.pathname = '/f';
    strictEqual(new URLPattern(init).pathname, '/f');
    // As is a URL, read through its getters.
    deepStrictEqual(components(new URLPattern(new URL('http://a.com/p'))), [
      'http',
      '',
      '',
      'a.com',
      '',
      '/p',
      '',
      '',
    ]);
  },
};

export const parseErrors = {
  test() {
    const syntax = 'Syntax error in URL Pattern';
    // [input, base, the legacy parser's message]
    const cases = [
      ['((', undefined, `${syntax}: a relative pattern must have a base URL.`],
      [{ pathname: '/(a[)' }, undefined, 'Invalid regular expression syntax.'],
      [
        { pathname: '/:a/:a' },
        undefined,
        `${syntax}: Duplicated part names [a]`,
      ],
      [
        { pathname: '/{a' },
        undefined,
        `${syntax}: Missing required close token`,
      ],
      [{ pathname: '/:' }, undefined, `${syntax}: invalid name start at 1`],
      [{ protocol: 'a b' }, undefined, syntax],
      ['/a', 'nope', 'Invalid base URL.'],
      [{ pathname: '/a', baseURL: 'nope' }, undefined, 'Invalid base URL.'],
    ];
    for (const [input, base, legacyMessage] of cases) {
      const make = () =>
        base === undefined
          ? new URLPattern(input)
          : new URLPattern(input, base);
      throws(
        make,
        urlPatternStandard ? kFailed : legacyError(legacyMessage),
        JSON.stringify(input)
      );
    }
  },
};

export const regExpFlags = {
  test() {
    // The standard parser compiles with the v flag, the legacy one with u.
    const vOnly = () => new URLPattern({ pathname: '/([\\p{L}--[a-z]])' });
    const uOnly = () => new URLPattern({ pathname: '/([a-z&&b])' });
    if (urlPatternStandard) {
      strictEqual(vOnly().test({ pathname: '/A' }), true);
      throws(uOnly, kFailed);
    } else {
      throws(vOnly, legacyError('Invalid regular expression syntax.'));
      strictEqual(uOnly().test({ pathname: '/&' }), true);
    }
  },
};

export const hasRegExpGroups = {
  test() {
    if (!urlPatternStandard) {
      strictEqual(new URLPattern({}).hasRegExpGroups, undefined);
      return;
    }
    strictEqual(new URLPattern({}).hasRegExpGroups, false);
    strictEqual(new URLPattern({ pathname: '/:a/*' }).hasRegExpGroups, false);
    strictEqual(new URLPattern({ pathname: '/(\\d+)' }).hasRegExpGroups, true);
    strictEqual(new URLPattern({ hash: ':a(b)' }).hasRegExpGroups, true);
  },
};
