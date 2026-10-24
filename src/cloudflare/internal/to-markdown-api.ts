// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { AiInternalError } from 'cloudflare-internal:ai-api';
import type { GatewayOptions } from 'cloudflare-internal:aig-api';

interface Fetcher {
  fetch: typeof fetch;
}

export type MarkdownDocument = {
  name: string;
  blob: Blob;
};

export type OutputFormat = 'markdown' | 'text';

export type ConversionResponse =
  | {
      id: string;
      name: string;
      mimeType: string;
      format: OutputFormat;
      tokens: number;
      data: string;
    }
  | {
      id: string;
      name: string;
      mimeType: string;
      format: 'error';
      error: string;
    };

export type ImageConversionOptions = {
  descriptionLanguage?: 'en' | 'es' | 'fr' | 'it' | 'pt' | 'de';
};

export type EmbeddedImageConversionOptions = ImageConversionOptions & {
  convert?: boolean;
  maxConvertedImages?: number;
};

export type ConversionOutputOptions = {
  format?: OutputFormat;
};

export type ConversionOptions = {
  output?: ConversionOutputOptions;
  html?: {
    images?: EmbeddedImageConversionOptions & { convertOGImage?: boolean };
    hostname?: string;
    cssSelector?: string;
  };
  docx?: {
    images?: EmbeddedImageConversionOptions;
  };
  image?: ImageConversionOptions;
  pdf?: {
    images?: EmbeddedImageConversionOptions;
    metadata?: boolean;
  };
};

export type ConversionRequestOptions = {
  gateway?: GatewayOptions;
  extraHeaders?: object;
  conversionOptions?: ConversionOptions;
};

export type SupportedFileFormat = {
  mimeType: string;
  extension: string;
};

// Captured when the module loads, before user code runs, so user code that replaces
// Uint8Array.prototype.toBase64 does not change the request body.
const uint8ArrayToBase64 = Function.prototype.call.bind(
  // eslint-disable-next-line @typescript-eslint/unbound-method
  Uint8Array.prototype.toBase64
) as (bytes: Uint8Array) => string;

async function blobToBase64(blob: Blob): Promise<string> {
  return uint8ArrayToBase64(new Uint8Array(await blob.arrayBuffer()));
}

export class ToMarkdownService {
  #fetcher: Fetcher;
  #endpointURL = 'https://workers-binding.ai';

  constructor(fetcher: Fetcher) {
    this.#fetcher = fetcher;
  }

  async transform(
    files: MarkdownDocument[],
    options?: ConversionRequestOptions
  ): Promise<ConversionResponse[]>;
  async transform(
    files: MarkdownDocument,
    options?: ConversionRequestOptions
  ): Promise<ConversionResponse>;
  async transform(
    files: MarkdownDocument | MarkdownDocument[],
    options?: ConversionRequestOptions
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

    // If the user sent a list of files, return an array of results, otherwise, return just the first object
    if (Array.isArray(files)) {
      return data.result;
    }

    if (data.result.length === 0) {
      throw new AiInternalError(
        'Internal Error Converting files into Markdown'
      );
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
