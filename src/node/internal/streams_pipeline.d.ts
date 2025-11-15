export { pipeline } from 'node:stream';

export function pipelineImpl(
  streams: unknown,
  callback: (err: Error | null, value?: unknown) => void,
  opts: { signal?: AbortSignal | undefined; end?: boolean | undefined }
): unknown;
