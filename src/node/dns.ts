import { default as dnsUtil } from 'node-internal:dns';
import * as errorCodes from 'node-internal:internal_dns_constants';

export * from 'node-internal:internal_dns_constants';

export const getServers = dnsUtil.getServers.bind(dnsUtil);
export const lookup = dnsUtil.lookup.bind(dnsUtil);
export const lookupService = dnsUtil.lookupService.bind(dnsUtil);
export const resolve = dnsUtil.resolve.bind(dnsUtil);
export const resolve4 = dnsUtil.resolve4.bind(dnsUtil);
export const resolve6 = dnsUtil.resolve6.bind(dnsUtil);
export const resolveAny = dnsUtil.resolveAny.bind(dnsUtil);
export const resolveCname = dnsUtil.resolveCname.bind(dnsUtil);
export const resolveCaa = dnsUtil.resolveCaa.bind(dnsUtil);
export const resolveMx = dnsUtil.resolveMx.bind(dnsUtil);
export const resolveNaptr = dnsUtil.resolveNaptr.bind(dnsUtil);
export const resolveNs = dnsUtil.resolveNs.bind(dnsUtil);
export const resolvePtr = dnsUtil.resolvePtr.bind(dnsUtil);
export const resolveSoa = dnsUtil.resolveSoa.bind(dnsUtil);
export const resolveSrv = dnsUtil.resolveSrv.bind(dnsUtil);
export const resolveTxt = dnsUtil.resolveTxt.bind(dnsUtil);
export const reverse = dnsUtil.reverse.bind(dnsUtil);
export const setDefaultResultOrder =
  dnsUtil.setDefaultResultOrder.bind(dnsUtil);
export const getDefaultResultOrder =
  dnsUtil.getDefaultResultOrder.bind(dnsUtil);
export const setServers = dnsUtil.setServers.bind(dnsUtil);

export default {
  getServers,
  lookup,
  lookupService,
  resolve,
  resolve4,
  resolve6,
  resolveAny,
  resolveCname,
  resolveCaa,
  resolveMx,
  resolveNaptr,
  resolveNs,
  resolvePtr,
  resolveSoa,
  resolveSrv,
  resolveTxt,
  reverse,
  setDefaultResultOrder,
  getDefaultResultOrder,
  setServers,

  ...errorCodes,
};
