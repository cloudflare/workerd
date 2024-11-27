import * as errorCodes from 'node-internal:internal_dns_constants';
import {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
} from 'node-internal:internal_dns';

export * from 'node-internal:internal_dns_constants';
export {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
} from 'node-internal:internal_dns';

export default {
  reverse,
  resolveTxt,
  resolveCaa,
  resolveMx,
  ...errorCodes,
};
