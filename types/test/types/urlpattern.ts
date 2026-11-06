// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

new URLPattern();
new URLPattern('/books/:id', 'https://example.com');
new URLPattern({ pathname: '/books/:id' }, { ignoreCase: true });
new URLPattern('/books/:id', 'https://example.com', { ignoreCase: true });
// @ts-expect-error ignoreCase is a boolean.
new URLPattern({}, { ignoreCase: 'yes' });

const pattern = new URLPattern({ pathname: '/books/:id' });
expectType<string>(pattern.pathname);
expectType<boolean>(pattern.hasRegExpGroups);
// @ts-expect-error the components are read-only.
pattern.pathname = '/x';

expectType<boolean>(pattern.test('https://example.com/books/1'));
expectType<boolean>(pattern.test({ pathname: '/books/1' }));
const result = pattern.exec('/books/1', 'https://example.com');
expectType<URLPatternResult | null>(result);
if (result !== null) {
  expectType<(string | URLPatternInit)[]>(result.inputs);
  expectType<string>(result.pathname.input);
  expectType<Record<string, string>>(result.pathname.groups);
}
