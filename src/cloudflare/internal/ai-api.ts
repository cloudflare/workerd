// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch
}

export type SessionOptions = {  // Deprecated, do not use this
  extraHeaders?: object;
};

export type AiOptions = {
  debug?: boolean;
  prefix?: string;
  extraHeaders?: object;
  /*
   * @deprecated this option is deprecated, do not use this
   */
  sessionOptions?: SessionOptions;
};

export class InferenceUpstreamError extends Error {
  public constructor(message: string) {
    super(message);
    this.name = "InferenceUpstreamError";
  }
}

export class Ai {
  private readonly fetcher: Fetcher

  private options: AiOptions = {};
  private logs: Array<string> = [];
  public lastRequestId: string | null = null;

  public constructor(fetcher: Fetcher) {
    this.fetcher = fetcher
  }

  public async fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    return this.fetcher.fetch(input, init)
  }

  public async run(
    model: string,
    inputs: Record<string, object>,
    options: AiOptions = {}
  ): Promise<Response | ReadableStream<Uint8Array> | object | null> {
    this.options = options;
    this.lastRequestId = "";

    const body = JSON.stringify({
      inputs,
      options: {
        debug: this.options?.debug,
      },
    });

    const fetchOptions = {
      method: "POST",
      body: body,
      headers: {
        ...(this.options?.sessionOptions?.extraHeaders || {}),
        ...(this.options?.extraHeaders || {}),
        "content-type": "application/json",
        // 'content-encoding': 'gzip',
        "cf-consn-sdk-version": "2.0.0",
        "cf-consn-model-id": `${this.options.prefix ? `${this.options.prefix}:` : ""}${model}`,
      },
    };

    const res = await this.fetcher.fetch("http://workers-binding.ai/run?version=2", fetchOptions);

    this.lastRequestId = res.headers.get("cf-ai-req-id");

    if (inputs['stream']) {
      if (!res.ok) {
        throw new InferenceUpstreamError(await res.text());
      }

      return res.body;
    } else {
      // load logs
      if (this.options.debug) {
        let parsedLogs: string[] = [];
        try {
          const logHeader = res.headers.get("cf-ai-logs")
          if (logHeader) {
            parsedLogs = (JSON.parse(atob(logHeader)) as string[]);
          }
        } catch {
          /* empty */
        }

        this.logs = parsedLogs;
      }

      if (!res.ok || !res.body) {
        throw new InferenceUpstreamError(await res.text());
      }

      const contentType = res.headers.get("content-type");

      if (contentType === "application/json") {
        return (await res.json() as object);
      }

      return res.body;
    }
  }

  public getLogs(): Array<string> {
    return this.logs;
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher)
}
