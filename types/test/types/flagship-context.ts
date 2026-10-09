// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

declare const flags: Flagship;

flags.getBooleanValue('dark-mode', false, {
  targetingKey: 'user-123',
  profile: { account: { plan: 'enterprise' } },
  tags: ['beta', 42, true, null],
  nullable: null,
});

const context: FlagshipEvaluationContext = {
  profile: { plan: 'enterprise' },
  groups: [['admin'], ['developer']],
};
flags.getStringDetails('checkout-flow', 'control', context);

// @ts-expect-error Date values must be serialized before calling the binding.
flags.getBooleanValue('dark-mode', false, { createdAt: new Date() });
// @ts-expect-error Functions are not valid evaluation context values.
flags.getBooleanValue('dark-mode', false, { callback: () => true });
// @ts-expect-error Undefined is represented by an absent context key.
flags.getBooleanValue('dark-mode', false, { missing: undefined });
