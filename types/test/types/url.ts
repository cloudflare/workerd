// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

const url = new URL('/path', new URL('https://example.com'));
expectType<string>(url.href);
expectType<string>(url.origin);
expectType<URLSearchParams>(url.searchParams);
url.hash = '#h';
// @ts-expect-error origin is read-only.
url.origin = 'https://example.org';

expectType<boolean>(URL.canParse('https://example.com', 'https://base/'));
expectType<URL | null>(URL.parse('https://example.com'));
expectType<string>(URL.createObjectURL(new Blob([])));
URL.revokeObjectURL('blob:https://example.com/id');

// The constructor takes the WebIDL union.
new URLSearchParams();
new URLSearchParams('a=1&b=2');
new URLSearchParams({ a: '1' });
new URLSearchParams([['a', '1']]);
new URLSearchParams(new Map([['a', '1']]));
new URLSearchParams(url.searchParams);
// @ts-expect-error a record's values are strings.
new URLSearchParams({ a: 1 });

const params = new URLSearchParams('a=1');
expectType<string | null>(params.get('a'));
expectType<string[]>(params.getAll('a'));
expectType<boolean>(params.has('a', '1'));
params.delete('a', '1');
expectType<number>(params.size);
expectType<IterableIterator<[key: string, value: string]>>(params.entries());
expectType<IterableIterator<string>>(params.keys());
expectType<IterableIterator<string>>(params.values());
for (const [name, value] of params) {
  expectType<string>(name);
  expectType<string>(value);
}
params.forEach(function (this: number, value, name, parent) {
  expectType<number>(this);
  expectType<string>(value);
  expectType<string>(name);
  expectType<URLSearchParams>(parent);
}, 1);
