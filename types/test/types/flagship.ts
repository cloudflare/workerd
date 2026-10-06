// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

type Equal<A, B> =
  (<T>() => T extends A ? 1 : 2) extends <T>() => T extends B ? 1 : 2
    ? true
    : false;
function assertType<T extends true>() {}
function expectType<T>(_value: T) {}

declare const flags: Flagship;

const booleanValue = flags.getValue('dark-mode', false);
const stringValue = flags.getValue('message', 'fallback');
const numberValue = flags.getValue('limit', 10);
const details = flags.getDetails('message', 'fallback', {
  targetingKey: 'user-123',
});

assertType<Equal<typeof booleanValue, Promise<boolean>>>();
assertType<Equal<typeof stringValue, Promise<string>>>();
assertType<Equal<typeof numberValue, Promise<number>>>();
assertType<
  Equal<typeof details, Promise<FlagshipEvaluationDetails<string>>>
>();

expectType<Promise<{ theme: string }>>(
  flags.getValue('config', { theme: 'light' })
);
expectType<Promise<number[]>>(flags.getValue('items', [1, 2, 3]));
expectType<Promise<FlagshipEvaluationDetails<{ theme: string }>>>(
  flags.getDetails('config', { theme: 'light' })
);

// @ts-expect-error The generic methods require a default value.
flags.getValue('dark-mode');
// @ts-expect-error The generic methods require a supported default value.
flags.getDetails('dark-mode', undefined);
