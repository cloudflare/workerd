// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare const ai: Ai;
const messages = [{ role: 'user' as const, content: 'Hello' }];

// Reasoning models accept exactly the reasoning controls they support: their
// supported efforts (compatibility aliases are documented, not typed), no
// effort property when they have no effort levels, and `enable_thinking: true`
// only when reasoning is mandatory. Models with their own schema (Nemotron) get
// exactly that schema's controls.

type Equal<A, B> =
  (<X>() => X extends A ? 1 : 2) extends <X>() => X extends B ? 1 : 2
    ? true
    : false;
type ChatEffort<Model extends keyof AiModels> =
  AiModels[Model]['inputs'] extends infer Input
    ? Input extends { reasoning_effort?: infer Effort }
      ? Exclude<Effort, null | undefined>
      : never
    : never;
type ResponsesEffort<Model extends keyof AiModels> =
  AiModels[Model]['inputs'] extends infer Input
    ? Input extends { reasoning?: infer Options }
      ? // Skip XOR's Chat branch, whose `reasoning` is `never`.
        [NonNullable<Options>] extends [never]
        ? never
        : NonNullable<Options> extends { effort?: infer Effort }
          ? Exclude<Effort, null | undefined>
          : never
      : never
    : never;

const glm52: Equal<
  ChatEffort<'@cf/zai-org/glm-5.2'>,
  'max' | 'high' | 'none'
> = true;
const dsv4: Equal<
  ChatEffort<'@cf/deepseek-ai/deepseek-v4-flash-0731'>,
  'max' | 'high' | 'low' | 'none'
> = true;
const glm53: Equal<
  ChatEffort<'@cf/zai-org/glm-5.3'>,
  'max' | 'high' | 'low'
> = true;
const glm53Flash: Equal<
  ChatEffort<'@cf/zai-org/glm-5.3-flash'>,
  'max' | 'high' | 'low'
> = true;
const kimi26: Equal<
  ChatEffort<'@cf/moonshotai/kimi-k2.6'>,
  'high' | 'none'
> = true;
const qwen: Equal<
  ChatEffort<'@cf/qwen/qwen3.8-27b'>,
  'low' | 'medium' | 'xhigh'
> = true;
const gptOssChat: Equal<
  ChatEffort<'@cf/openai/gpt-oss-20b'>,
  'low' | 'medium' | 'high'
> = true;
const gptOssResponses: Equal<
  ResponsesEffort<'@cf/openai/gpt-oss-20b'>,
  'low' | 'medium' | 'high'
> = true;
// No effort levels, or the model's own schema: no reasoning_effort at all.
const glm47: Equal<ChatEffort<'@cf/zai-org/glm-4.7-flash'>, never> = true;
const gemma: Equal<ChatEffort<'@cf/google/gemma-4-26b-a4b-it'>, never> = true;
const kimiCode: Equal<
  ChatEffort<'@cf/moonshotai/kimi-k2.7-code'>,
  never
> = true;
const nemotron: Equal<
  ChatEffort<'@cf/nvidia/nemotron-3-120b-a12b'>,
  never
> = true;
// Models without reasoning metadata keep the shared type.
const shared: Equal<
  ChatEffort<'@cf/moonshotai/kimi-k2.5'>,
  'low' | 'medium' | 'high'
> = true;
void [
  glm52,
  dsv4,
  glm53,
  glm53Flash,
  kimi26,
  qwen,
  gptOssChat,
  gptOssResponses,
  glm47,
  gemma,
  kimiCode,
  nemotron,
  shared,
];

// Supported controls type-check.
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: 'max' });
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: null });
void ai.run('@cf/zai-org/glm-5.2', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});
void ai.run('@cf/qwen/qwen3.8-27b', { messages, reasoning_effort: 'xhigh' });
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  reasoning: { effort: 'low' },
});
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  skip_special_tokens: true,
  chat_template_kwargs: { enable_thinking: false },
});
void ai.run('@cf/zai-org/glm-4.7-flash', {
  messages,
  chat_template_kwargs: { enable_thinking: false, clear_thinking: false },
});
void ai.run('@cf/zai-org/glm-5.3', {
  messages,
  reasoning_effort: 'low',
  chat_template_kwargs: { enable_thinking: true },
});
void ai.run('@cf/nvidia/nemotron-3-120b-a12b', {
  messages,
  chat_template_kwargs: {
    enable_thinking: true,
    low_effort: true,
    force_nonempty_content: true,
  },
});
declare const glm53Effort: 'max' | 'high' | 'low';
void ai.run('@cf/zai-org/glm-5.3', { messages, reasoning_effort: glm53Effort });

// Other model options are unaffected.
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  service_tier: 'priority',
});
declare const chatInput: ChatCompletionsInput;
void ai.run('@cf/moonshotai/kimi-k2.5', chatInput);
const sharedReasoning: Reasoning = { effort: 'high' };
const sharedOptions: ChatCompletionsCommonOptions = { reasoning_effort: 'low' };
const sharedKwargs: ChatTemplateKwargs = { enable_thinking: false };
void [sharedReasoning, sharedOptions, sharedKwargs];

// The original Gemma class name remains available.
const legacyGemma: Base_Ai_Cf_Google_Gemma_4_26B_A4B_IT['inputs'] = {
  messages,
};
const gemmaInputs: AiModels['@cf/google/gemma-4-26b-a4b-it']['inputs'] =
  legacyGemma;
void gemmaInputs;

// Efforts a model does not support are rejected.
// @ts-expect-error: GPT-OSS does not support "max"
void ai.run('@cf/openai/gpt-oss-20b', { messages, reasoning_effort: 'max' });
// @ts-expect-error: Qwen3.8 does not support "high"
void ai.run('@cf/qwen/qwen3.8-27b', { messages, reasoning_effort: 'high' });
// @ts-expect-error: GLM-5.3 reasoning cannot be turned off
void ai.run('@cf/zai-org/glm-5.3', { messages, reasoning_effort: 'none' });
// @ts-expect-error: aliases are accepted by the API, not by the types
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: 'low' });
// @ts-expect-error: unknown efforts are rejected
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: 'turbo' });
declare const anyEffort: string;
// @ts-expect-error: arbitrary strings are rejected
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: anyEffort });
declare const legacyEffort: 'low' | 'medium' | 'high';
// @ts-expect-error: the shared effort type includes efforts GLM-5.3 lacks
void ai.run('@cf/zai-org/glm-5.3', {
  messages,
  reasoning_effort: legacyEffort,
});
// @ts-expect-error: the shared request type allows controls Qwen3.8 lacks
void ai.run('@cf/qwen/qwen3.8-27b', chatInput);
// @ts-expect-error: Responses efforts are exact too
void ai.run('@cf/openai/gpt-oss-20b', {
  input: 'Hello',
  reasoning: { effort: 'minimal' },
});

// Models without effort levels, or with their own controls, have no effort.
// @ts-expect-error: Gemma 4 has no effort levels
void ai.run('@cf/google/gemma-4-26b-a4b-it', {
  messages,
  reasoning_effort: 'low',
});
// @ts-expect-error: Kimi K2.7 Code has no effort levels
void ai.run('@cf/moonshotai/kimi-k2.7-code', {
  messages,
  reasoning_effort: 'high',
});
// @ts-expect-error: Nemotron has no top-level reasoning_effort
void ai.run('@cf/nvidia/nemotron-3-120b-a12b', {
  messages,
  reasoning_effort: 'low',
});
// @ts-expect-error: Nemotron's chat_template_kwargs are its own
void ai.run('@cf/nvidia/nemotron-3-120b-a12b', {
  messages,
  chat_template_kwargs: { clear_thinking: true },
});

// Mandatory reasoning cannot be turned off.
// @ts-expect-error: GPT-OSS reasoning is always on
void ai.run('@cf/openai/gpt-oss-20b', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});
// @ts-expect-error: Kimi K2.7 Code reasoning is always on
void ai.run('@cf/moonshotai/kimi-k2.7-code', {
  messages,
  chat_template_kwargs: { enable_thinking: false },
});

// New models are added and models no longer offered are removed.
const speech: keyof AiModels = '@cf/nvidia/nemotron-speech-streaming-en-0.6b';
// @ts-expect-error: stable-diffusion-v1-5-img2img is no longer offered
const img2img: keyof AiModels = '@cf/runwayml/stable-diffusion-v1-5-img2img';
void [speech, img2img];

// Types still reject values of the wrong kind.
// @ts-expect-error: efforts are strings
void ai.run('@cf/zai-org/glm-5.2', { messages, reasoning_effort: 1 });
// @ts-expect-error: Responses and Chat Completions inputs are mutually exclusive
void ai.run('@cf/openai/gpt-oss-20b', { input: 'Hello', messages });
