import { AiInternalError } from 'cloudflare-internal:ai-api';
import type { GatewayOptions } from 'cloudflare-internal:aig-api';

// TODO(soon): Remove this once TypeScript recognizes base64/hex methods
// Type declarations for V8 14.1+ ArrayBuffer base64/hex methods
// https://tc39.es/proposal-arraybuffer-base64/
declare global {
  interface Uint8Array {
    toHex(): string;
    toBase64(options?: {
      alphabet?: 'base64' | 'base64url';
      omitPadding?: boolean;
    }): string;
    setFromHex(hexString: string): { read: number; written: number };
    setFromBase64(
      base64String: string,
      options?: {
        alphabet?: 'base64' | 'base64url';
        lastChunkHandling?: 'loose' | 'strict' | 'stop-before-partial';
      }
    ): { read: number; written: number };
  }
  interface Uint8ArrayConstructor {
    fromHex(hexString: string): Uint8Array;
    fromBase64(
      base64String: string,
      options?: {
        alphabet?: 'base64' | 'base64url';
        lastChunkHandling?: 'loose' | 'strict' | 'stop-before-partial';
      }
    ): Uint8Array;
  }
}

interface Fetcher {
  fetch: typeof fetch;
}

export type ConversionResponse = {
  name: string;
  mimeType: string;
} & (
  | {
      format: 'markdown';
      tokens: number;
      data: string;
    }
  | {
      format: 'error';
      error: string;
    }
);

export type SupportedFileFormat = {
  mimeType: string;
  extension: string;
};

async function blobToBase64(blob: Blob): Promise<string> {
  const arrayBuffer = await blob.arrayBuffer();
  return new Uint8Array(arrayBuffer).toBase64();
}

export class ToMarkdownService {
  #fetcher: Fetcher;
  #endpointURL = 'https://workers-binding.ai';

  constructor(fetcher: Fetcher) {
    this.#fetcher = fetcher;
  }

  async transform(
    files: { name: string; blob: Blob }[],
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
  ): Promise<ConversionResponse[]>;
  async transform(
    files: {
      name: string;
      blob: Blob;
    },
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
  ): Promise<ConversionResponse>;
  async transform(
    files: { name: string; blob: Blob } | { name: string; blob: Blob }[],
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
  ): Promise<ConversionResponse | ConversionResponse[]> {
    const input = Array.isArray(files) ? files : [files];

    const processedFiles = [];
    for (const file of input) {
      processedFiles.push({
        name: file.name,
        mimeType: file.blob.type,
        data: await blobToBase64(file.blob),
      });
    }

    const fetchOptions = {
      method: 'POST',
      body: JSON.stringify({
        files: processedFiles,
        options: options,
      }),
      headers: {
        ...options?.extraHeaders,
        'content-type': 'application/json',
      },
    };

    const endpointUrl = `${this.#endpointURL}/to-everything/markdown/transformer`;

    const res = await this.#fetcher.fetch(endpointUrl, fetchOptions);

    if (!res.ok) {
      const content = (await res.json()) as {
        errors: { message: string }[];
      };

      throw new AiInternalError(
        content.errors.at(0)?.message || 'Internal Error'
      );
    }

    const data = (await res.json()) as { result: ConversionResponse[] };

    if (data.result.length === 0) {
      throw new AiInternalError(
        'Internal Error Converting files into Markdown'
      );
    }

    // If the user sent a list of files, return an array of results, otherwise, return just the first object
    if (Array.isArray(files)) {
      return data.result;
    }

    const obj = data.result.at(0);
    if (!obj) {
      throw new AiInternalError(
        'Internal Error Converting files into Markdown'
      );
    }

    return obj;
  }

  async supported(): Promise<SupportedFileFormat[]> {
    const fetchOptions = {
      method: 'GET',
      headers: {
        'content-type': 'application/json',
      },
    };

    const endpointUrl = `${this.#endpointURL}/to-everything/markdown/supported`;

    const res = await this.#fetcher.fetch(endpointUrl, fetchOptions);

    // This endpoint never fails so just parse the output
    const { result: extensions } = (await res.json()) as {
      result: SupportedFileFormat[];
    };

    return extensions;
  }
}
