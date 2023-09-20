import { default as inner } from 'cloudflare-internal:request-context';

export function getRequestContext() {
  return inner.getRequestContext();
};

export default getRequestContext;
