// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare const ai: Ai;
const messages = [{ role: 'user' as const, content: 'Hello' }];

// Reasoning types suggest each model's supported efforts but never reject a
// value that type-checked before: any string effort and any boolean
// enable_thinking are accepted for every model.

type Equal<A, B> =
  (<X>() => X extends A ? 1 : 2) extends <X>() => X extends B ? 1 : 2
    ? true
    : false;
// The literal suggestions an effort type offers (drops the open `string` part).
type Suggested<T> = T extends string ? (string extends T ? never : T) : never;
type ChatEffort<Model extends keyof AiModels> =
  AiModels[Model]['inputs'] extends infer Input
    ? Input extends { reasoning_effort?: infer Effort }
      ? Suggested<Exclude<Effort, null | undefined>>
      : never
    : never;
type ResponsesEffort<Model extends keyof AiModels> =
  AiModels[Model]['inputs'] extends infer Input
    ? Input extends { reasoning?: infer Options }
      ? NonNullable<Options> extends { effort?: infer Effort }
        ? Suggested<Exclude<Effort, null | undefined>>
        : never
      : never
    : never;

// Suggestions are the canonical supported efforts from model metadata.
const glm52: Equal<
  ChatEffort<'@cf/zai-org/glm-5.2'>,
  'max' | 'high' | 'none'
> = true;
const qwen: Equal<
  ChatEffort<'@cf/qwen/qwen3.8-27b'>,
  'low' | 'medium' | 'xhigh'
> = true;
const gptOss: Equal<
  ResponsesEffort<'@cf/openai/gpt-oss-20b'>,
  'low' | 'medium' | 'high'
> = true;
// Toggle-only models suggest no effort levels.
const glm47: Equal<ChatEffort<'@cf/zai-org/glm-4.7-flash'>, never> = true;
const gemma: Equal<ChatEffort<'@cf/google/gemma-4-26b-a4b-it'>, never> = true;
// Models without metadata keep the shared suggestions.
const shared: Equal<
  ChatEffort<'@cf/nvidia/nemotron-3-120b-a12b'>,
  'low' | 'medium' | 'high'
> = true;
void [glm52, qwen, gptOss, glm47, gemma, shared];

// Values outside the suggestions still type-check.
void ai.run('@cf/qwen/qwen3.8-27b', { messages, reasoning_effort: 'high' });
void ai.run('@cf/qwen/qwen3.8-27b', { messages, reasoning_effort: 'none' });
void ai.run('@cf/zai-org/glm-4.7-flash', { messages, reasoning_effort: 'low' });
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  reasoning_effort: 'none',
});
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  reasoning_effort: 'turbo',
});
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: 'medium' });
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: null });
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  reasoning: { effort: 'minimal' },
});
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  reasoning: { effort: 'none' },
});

// enable_thinking stays boolean, including for models with mandatory reasoning.
void ai.run('@cf/moonshotai/kimi-k2.7-code', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});
void ai.run('@cf/zai-org/glm-5.3', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});
void ai.run('@cf/openai/gpt-oss-20b', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});
void ai.run('@cf/zai-org/glm-4.7-flash', {
  messages,
  chat_template_kwargs: { enable_thinking: false, clear_thinking: false },
});

// Values typed with the shared types are accepted by every model.
declare const effort: string;
declare const flag: boolean;
declare const chatInput: ChatCompletionsInput;
declare const responsesInput: ResponsesInput;
void ai.run('@cf/zai-org/glm-5.2', {
  messages,
  reasoning_effort: effort,
  chat_template_kwargs: { enable_thinking: flag },
});
void ai.run('@cf/qwen/qwen3.8-27b', chatInput);
void ai.run('@cf/moonshotai/kimi-k2.6', chatInput);
void ai.run('@cf/google/gemma-4-26b-a4b-it', chatInput);
void ai.run('@cf/openai/gpt-oss-120b', chatInput);
void ai.run('@cf/openai/gpt-oss-120b', responsesInput);
const sharedReasoning: Reasoning = { effort: 'high' };
const sharedOptions: ChatCompletionsCommonOptions = { reasoning_effort: 'max' };
const sharedKwargs: ChatTemplateKwargs = { enable_thinking: false };
void [sharedReasoning, sharedOptions, sharedKwargs];

// Other model-specific options are unaffected.
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  skip_special_tokens: true,
  service_tier: 'priority',
});
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  service_tier: 'priority',
});

// The original Gemma class name remains available for existing code.
const legacyGemma: Base_Ai_Cf_Google_Gemma_4_26B_A4B_IT['inputs'] = {
  messages,
  reasoning_effort: 'none',
};
const gemmaInputs: AiModels['@cf/google/gemma-4-26b-a4b-it']['inputs'] =
  legacyGemma;
void gemmaInputs;

// Types still reject values of the wrong kind.
// @ts-expect-error: efforts are strings
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: 1 });
// @ts-expect-error: enable_thinking is a boolean
void ai.run('@cf/moonshotai/kimi-k2.7-code', {
  messages,
  chat_template_kwargs: { enable_thinking: 'no' },
});
// @ts-expect-error: Responses and Chat Completions inputs are mutually exclusive
void ai.run('@cf/openai/gpt-oss-20b', { input: 'Hello', messages });
