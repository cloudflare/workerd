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

export declare abstract class ToMarkdownService {
  transform(
    files: { name: string; blob: Blob }[],
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
  ): Promise<ConversionResponse[]>;
  transform(
    files: {
      name: string;
      blob: Blob;
    },
    options?: { gateway?: GatewayOptions; extraHeaders?: object }
  ): Promise<ConversionResponse>;
  supported(): Promise<SupportedFileFormat[]>;
}
