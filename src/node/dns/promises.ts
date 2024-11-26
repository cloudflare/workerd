import * as errorCodes from 'node-internal:internal_dns_constants';
import { default as dnsUtil } from 'node-internal:dns';

export * from 'node-internal:internal_dns_constants';
export const Resolver = dnsUtil.Resolver;

export default {
  ...errorCodes,
  Resolver,
};
