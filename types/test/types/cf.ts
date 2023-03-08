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
