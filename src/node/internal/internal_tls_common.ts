import type tls from 'node:tls';
import { ERR_OPTION_NOT_IMPLEMENTED } from 'node-internal:internal_errors';
import { validateInteger } from 'node-internal:validators';

export class SecureContext {
  public context: unknown = undefined;
  public constructor(
    _secureProtocol?: string,
    secureOptions?: number,
    minVersion?: string,
    maxVersion?: string
  ) {
    if (minVersion !== undefined) {
      throw new ERR_OPTION_NOT_IMPLEMENTED('minVersion');
    }
    if (maxVersion !== undefined) {
      throw new ERR_OPTION_NOT_IMPLEMENTED('maxVersion');
    }
    if (secureOptions) {
      validateInteger(secureOptions, 'secureOptions');
    }
  }
}

export function createSecureContext(
  options: tls.SecureContextOptions = {}
): SecureContext {
  const nonNullEntry = Object.entries(options).find(
    ([_key, value]) => value != null
  );
  if (nonNullEntry) {
    throw new ERR_OPTION_NOT_IMPLEMENTED(`options.${nonNullEntry[0]}`);
  }
  return new SecureContext(
    options.secureProtocol,
    options.secureOptions,
    options.minVersion,
    options.maxVersion
  );
}
