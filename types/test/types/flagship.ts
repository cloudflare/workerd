// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

declare const flags: Flagship;

expectType<Promise<boolean>>(flags.getValue('dark-mode', false));
expectType<Promise<string>>(flags.getValue('message', 'fallback'));
expectType<Promise<number>>(flags.getValue('limit', 10));
expectType<Promise<{ theme: string }>>(
  flags.getValue('config', { theme: 'light' })
);
expectType<Promise<number[]>>(flags.getValue('items', [1, 2, 3]));

expectType<Promise<FlagshipEvaluationDetails<boolean>>>(
  flags.getDetails('dark-mode', false, { targetingKey: 'user-123' })
);
expectType<Promise<FlagshipEvaluationDetails<{ theme: string }>>>(
  flags.getDetails('config', { theme: 'light' })
);

// @ts-expect-error The generic methods require a default value.
flags.getValue('dark-mode');
// @ts-expect-error The generic methods require a supported default value.
flags.getDetails('dark-mode', undefined);
