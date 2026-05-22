// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}


export const handler: ExportedHandler<{ AI: Ai }> = {
  async fetch(_request, env) {
    // Known model -- normal response
    {
      const result = await env.AI.run('@cf/meta/llama-3.1-8b-instruct-fp8', {
        prompt: 'hello',
      });
      expectType<AiTextGenerationOutput>(result);
    }

    // Known model -- streaming
    {
      const result = await env.AI.run('@cf/meta/llama-3.1-8b-instruct-fp8', {
        prompt: 'hello',
        stream: true as const,
      });
      expectType<ReadableStream>(result);
    }

    // Known model -- raw response
    {
      const result = await env.AI.run(
        '@cf/meta/llama-3.1-8b-instruct-fp8',
        { prompt: 'hello' },
        { returnRawResponse: true as const }
      );
      expectType<Response>(result);
    }

    // Known model -- batch request
    {
      const result = await env.AI.run(
        '@cf/meta/llama-3.1-8b-instruct-fp8',
        { requests: [{ prompt: 'hello' }, { prompt: 'world' }] },
        { queueRequest: true as const }
      );
      expectType<AiAsyncBatchResponse>(result);
    }

    // Gateway model -- unknown model name, permissive types
    {
      const result = await env.AI.run('google/nano-banana', {
        prompt: 'hello',
        aspect_ratio: '16:9',
      });
      expectType<Record<string, unknown>>(result);
    }

    // Gateway model with gateway options
    {
      const result = await env.AI.run(
        'google/nano-banana',
        { prompt: 'hello' },
        { gateway: { id: 'my-gateway' } }
      );
      expectType<Record<string, unknown>>(result);
    }

    // Known model names do not silently fall through to the unknown-model
    // gateway-fallback overload. The fallback's signature excludes
    // `keyof AiModelList`, so a call with a known model name and an input
    // shape that doesn't match the known-model overload must surface as a
    // type error rather than degrading to `Record<string, unknown>`. Here
    // we exercise that with kimi-k2.6, whose typed input is
    // `ChatCompletionsMessagesInput` (no `prompt` field).
    {
      // @ts-expect-error: kimi-k2.6 takes `messages`, not `prompt`; the
      // unknown-model fallback must not catch this call.
      await env.AI.run('@cf/moonshotai/kimi-k2.6', { prompt: 'hello' });
    }

    return new Response();
  },
};
