// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

export const handler: ExportedHandler<{ SERVICE: Fetcher }> = {
  async fetch(request, env) {
    // Can access incoming cf properties without narrowing
    expectType<string | undefined>(request.cf?.colo);
    // ...but cannot access request init cf properties
    expectType<unknown>(request.cf?.cacheEverything);

    // Can forward request as is (unknown cf properties ignored)
    await fetch(request);

    // Can forward request to different URL  (unknown cf properties ignored)
    await fetch("https://example.com", request);

    // Can fetch with request init cf properties
    await fetch("https://example.com", {
      cf: { cacheEverything: true },
    });

    await fetch("https://example.com", {
      cf: {
        vary: {
          default: { action: "bypass" },
          headers: {
            accept: {
              action: "normalize",
              media_types: ["image/webp", "image/avif"],
            },
            "accept-language": {
              action: "normalize",
              languages: ["en", "fr"],
            },
            "x-custom-header": { action: "passthrough" },
          },
        },
      },
    });

    expectType<RequestInitCfPropertiesVary>({
      default: { action: "bypass" },
      headers: {
        accept: {
          action: "normalize",
          media_types: ["image/webp"],
        },
        "accept-language": {
          action: "normalize",
          languages: ["en"],
        },
        "x-custom-header": { action: "normalize" },
      },
    });

    // @ts-expect-error: default is required for vary
    const varyWithoutDefault: RequestInitCfPropertiesVary = {
      headers: {
        accept: { action: "normalize" },
      },
    };

    const varyInvalidAction: RequestInitCfPropertiesVary = {
      // @ts-expect-error: action must be normalize, passthrough, or bypass
      default: { action: "invalid" },
    };

    const varyInvalidValues: RequestInitCfPropertiesVary = {
      default: { action: "bypass" },
      headers: {
        accept: {
          action: "normalize",
          media_types: [
            // @ts-expect-error: media_types values must be strings
            123,
          ],
        },
        "accept-language": {
          action: "normalize",
          languages: [
            // @ts-expect-error: languages values must be strings
            123,
          ],
        },
      },
    };
    expectType<RequestInitCfPropertiesVary>(varyWithoutDefault);
    expectType<RequestInitCfPropertiesVary>(varyInvalidAction);
    expectType<RequestInitCfPropertiesVary>(varyInvalidValues);

    // Can fetch to service binding with incoming properties to simulate incoming request
    await env.SERVICE.fetch("https://example.com", {
      cf: { colo: "LHR" },
    });

    // Can fetch to service binding with custom properties
    await env.SERVICE.fetch("https://example.com", {
      cf: { token: "thing" },
    });
    // ...and can safely access that on the incoming request
    expectType<unknown>(request.cf?.token);
    if (typeof request.cf?.token === "string") {
      expectType<string>(request.cf.token);
    }

    return new Response();
  },
};
