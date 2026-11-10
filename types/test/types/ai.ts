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

    // AI Gateway web search
    {
      const result = await env.AI.websearch({
        gatewayId: 'my-gateway',
        provider: 'exa',
        query: 'Cloudflare Workers',
        limit: 5,
        byokAlias: 'default',
      });
      expectType<Response>(result);
    }

    // The unified catalog is distinct from Workers AI's models() search.
    {
      const page = await env.AI.listModels({
        page: 1,
        per_page: 50,
        modality: 'text-generation',
      });
      expectType<AiCatalogModel[]>(page.result);
      expectType<number>(page.result_info.total_count);
      expectType<'USD'>(page.result[0]!.pricing.currency);
      expectType<string>(page.result[0]!.author);
      const detail = await env.AI.getModel('anthropic/claude');
      expectType<AiCatalogProvider[]>(detail.result.providers);
      expectType<boolean>(detail.result.providers[0]!.is_default);
      expectType<number | undefined>(
        detail.result.providers[0]!.performance?.latency_ms.p95
      );
      const gatewayPage = await env.AI.listModels({
        gatewayId: 'my-gateway',
        page: 1,
      });
      expectType<AiCatalogModel[]>(gatewayPage.result);
      const gatewayDetail = await env.AI.getModel('anthropic/claude', {
        gatewayId: 'my-gateway',
      });
      expectType<AiCatalogProvider[]>(gatewayDetail.result.providers);
      // @ts-expect-error the list query must use a numeric page
      env.AI.listModels({ page: '1' });

      const pricing: AiCatalogPricing = {
        type: 'tokens',
        currency: 'USD',
        unit: 'per million tokens',
        input_tokens: 15,
        output_tokens: 75,
      };
      const model: AiCatalogModel = {
        id: 'anthropic/claude',
        name: 'Claude',
        author: 'anthropic',
        source: 'aig-catalog',
        modality: 'text-generation',
        tags: ['LLM'],
        description: 'Claude language model',
        properties: { contextLength: 200000 },
        pricing,
        created_at: '2026-11-04T20:00:00Z',
      };
      const provider: AiCatalogProvider = {
        id: 'anthropic',
        name: 'Anthropic',
        slug: 'anthropic',
        is_default: true,
        is_custom: false,
        account_specific: false,
        pricing,
      };
      expectType<AiCatalogDetailResponse>({
        success: true,
        result: { ...model, providers: [provider] },
      });
      // @ts-expect-error catalog models require pricing and identity metadata
      expectType<AiCatalogModel>({ id: 'anthropic/claude', name: 'Claude' });
      // @ts-expect-error detail providers require pricing and provider metadata
      expectType<AiCatalogProvider>({ id: 'anthropic' });
      // @ts-expect-error unsupported pricing types are not part of the catalog contract
      expectType<AiCatalogPricing>({ ...pricing, type: 'neurons' });
      // @ts-expect-error detail responses must include providers
      expectType<AiCatalogDetailResponse>({ success: true, result: model });
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
