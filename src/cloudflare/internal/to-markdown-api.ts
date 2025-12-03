import { AiInternalError } from 'cloudflare-internal:ai-api';
import type { GatewayOptions } from 'cloudflare-internal:aig-api';
import base64 from 'cloudflare-internal:base64';

interface Fetcher {
  fetch: typeof fetch;
}

export type MarkdownDocument = {
  name: string;
  blob: Blob;
};

export type ConversionResponse =
  | {
      name: string;
      mimeType: string;
      format: 'markdown';
      tokens: number;
      data: string;
    }
  | {
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

export type ConversionOptions = {
  html?: {
    images?: EmbeddedImageConversionOptions & { convertOGImage?: boolean };
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

async function blobToBase64(blob: Blob): Promise<string> {
  return base64.encodeArrayToString(await blob.arrayBuffer());
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
