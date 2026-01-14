export type MarkdownDocument = {
  name: string
  blob: Blob
}

export type ConversionResponse =
  | {
      name: string
      mimeType: string
      format: 'markdown'
      tokens: number
      data: string
    }
  | {
      name: string
      mimeType: string
      format: 'error'
      error: string
    }

export type ImageConversionOptions = {
  descriptionLanguage?: 'en' | 'es' | 'fr' | 'it' | 'pt' | 'de'
}

export type EmbeddedImageConversionOptions = ImageConversionOptions & {
  convert?: boolean
  maxConvertedImages?: number
}

export type ConversionOptions = {
  html?: {
    images?: EmbeddedImageConversionOptions & { convertOGImage?: boolean }
  }
  docx?: {
    images?: EmbeddedImageConversionOptions
  }
  image?: ImageConversionOptions
  pdf?: {
    images?: EmbeddedImageConversionOptions
    metadata?: boolean
  }
}

export type ConversionRequestOptions = {
  gateway?: GatewayOptions
  extraHeaders?: object
  conversionOptions?: ConversionOptions
}

export type SupportedFileFormat = {
  mimeType: string
  extension: string
}

export declare abstract class ToMarkdownService {
  transform(
    files: MarkdownDocument[],
    options?: ConversionRequestOptions,
  ): Promise<ConversionResponse[]>
  transform(
    files: MarkdownDocument,
    options?: ConversionRequestOptions,
  ): Promise<ConversionResponse>
  supported(): Promise<SupportedFileFormat[]>
}
