// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare const ai: Ai;
const messages = [{ role: 'user' as const, content: 'Hello' }];

// Canonical controls and compatibility aliases remain accepted.
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  reasoning_effort: 'none',
});
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  reasoning_effort: 'auto',
});
void ai.run('@cf/moonshotai/kimi-k2.6', {
  messages,
  reasoning_effort: 'medium',
});
void ai.run('@cf/zai-org/glm-5.3', { messages, reasoning_effort: 'max' });
void ai.run('@cf/zai-org/glm-5.3-flash', { messages, reasoning_effort: 'max' });
void ai.run('@cf/nvidia/nemotron-3-120b-a12b', {
  messages,
  reasoning_effort: 'none',
});
void ai.run('@cf/qwen/qwen3.8-27b', {
  messages,
  reasoning_effort: 'xhigh',
});
void ai.run('@cf/deepseek-ai/deepseek-v4-pro-0813', {
  messages,
  reasoning_effort: 'low',
});
void ai.run('@cf/zai-org/glm-4.7-flash', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});

void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  reasoning: { effort: 'medium' },
});
void ai.run('@cf/openai/gpt-oss-120b', { messages, reasoning_effort: 'high' });
// A legacy alias is an accepted input even when it does not disable reasoning.
void ai.run('@cf/moonshotai/kimi-k2.7-code', {
  messages,
  reasoning_effort: 'none',
});

// @ts-expect-error: an unsupported effort must not reach the gateway fallback overload
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  reasoning_effort: 'turbo',
});
// @ts-expect-error: GPT-OSS reasoning is mandatory
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  reasoning: { effort: 'none' },
});
// @ts-expect-error: mandatory reasoning cannot be disabled with enable_thinking
void ai.run('@cf/moonshotai/kimi-k2.7-code', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});
// @ts-expect-error: Nemotron does not accept high reasoning effort
void ai.run('@cf/nvidia/nemotron-3-120b-a12b', {
  messages,
  reasoning_effort: 'high',
});
// @ts-expect-error: Qwen 3.8 does not accept none reasoning effort
void ai.run('@cf/qwen/qwen3.8-27b', {
  messages,
  reasoning_effort: 'none',
});

// Unrelated options remain available on both input formats.
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  service_tier: 'priority',
});
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  service_tier: 'priority',
});
