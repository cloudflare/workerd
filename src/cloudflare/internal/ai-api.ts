// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch
}

export type AiOptions = {
  debug?: boolean;
  prefix?: string;
  extraHeaders?: object;
};

export class InferenceUpstreamError extends Error {
  httpCode: number;

  constructor(message: string, httpCode: number) {
    super(message);
    this.name = "InferenceUpstreamError";
    this.httpCode = httpCode;
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

  async fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    return this.fetcher.fetch(input, init)
  }

  async run(
    model: string,
    inputs: any,
    options: AiOptions = {}
  ): Promise<any> {
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
        // @ts-ignore Backwards sessionOptions compatibility
        ...(this.options?.sessionOptions?.extraHeaders || {}),
        ...(this.options?.extraHeaders || {}),
        "content-encoding": "application/json",
        // 'content-encoding': 'gzip',
        "cf-consn-sdk-version": "2.0.0",
        "cf-consn-model-id": `${this.options.prefix ? `${this.options.prefix}:` : ""}${model}`,
      },
    };

    const res = await this.fetcher.fetch("http://workers-binding.ai/run?version=2", fetchOptions);

    this.lastRequestId = res.headers.get("cf-ai-req-id");

    if ((inputs as any).stream) {
      if (!res.ok) {
        throw new InferenceUpstreamError(await res.text(), res.status);
      }

      return res.body;
    } else {
      // load logs
      if (this.options.debug) {
        let parsedLogs = [];
        try {
          const logHeader = res.headers.get("cf-ai-logs")
          if (logHeader) {
            parsedLogs = JSON.parse(atob(logHeader));
          }
        } catch (e) {
          /* empty */
        }

        this.logs = parsedLogs;
      }

      if (!res.ok) {
        throw new InferenceUpstreamError(await res.text(), res.status);
      }

      // Non streaming responses are always in gzip
      // @ts-ignore
      const decompressed = await new Response(res.body.pipeThrough(new DecompressionStream("gzip")));

      const contentType = res.headers.get("content-type");

      // This is a hack to handle local mode requests using wrangler@3.30.1 or bellow
      // In this wrangler version, headers are not returning to customers worker, so it must try each
      if (!contentType) {
        console.log(
          "Your current wrangler version has a known issue when using in local dev mode, please update to the latest.",
        );

        try {
          return await decompressed.clone().json();
        } catch (e) {
          return decompressed.body;
        }
      }

      if (res.headers.get("content-type") === "application/json") {
        return await decompressed.json();
      }

      return decompressed.body;
    }
  }

  getLogs() {
    return this.logs;
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): Ai {
  return new Ai(env.fetcher)
}
