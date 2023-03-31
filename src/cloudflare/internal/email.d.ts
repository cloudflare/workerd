// Type definitions for c++ implementation.

export class EmailMessage {
  public constructor(from: string, to: string, raw: ReadableStream | string);
  public readonly from: string;
  public readonly to: string;
}
