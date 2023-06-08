import requestCtx from 'cloudflare-internal:request-context';
export function getRequestId(): string {
  return requestCtx.getRequestId();
}
