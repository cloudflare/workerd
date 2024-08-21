import { default as zlibUtil } from 'node-internal:zlib';
import { Buffer } from 'node-internal:internal_buffer';
import { validateUint32 } from 'node-internal:validators';
import { isArrayBufferView } from 'node-internal:internal_types';
import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';

function crc32(data: ArrayBufferView | string, value: number = 0): number {
  if (typeof data === 'string') {
    data = Buffer.from(data);
  } else if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE('data', 'ArrayBufferView', typeof data);
  }
  validateUint32(value, 'value');
  return zlibUtil.crc32(data, value);
}

const constPrefix = 'CONST_';
const constants = {};

Object.defineProperties(
  constants,
  Object.fromEntries(
    // eslint-disable-next-line @typescript-eslint/no-unsafe-argument
    Object.entries(Object.getPrototypeOf(zlibUtil))
      .filter(([k]) => k.startsWith(constPrefix))
      .map(([k, v]) => [
        k.slice(constPrefix.length),
        {
          value: v,
          writable: false,
          configurable: false,
          enumerable: true,
        },
      ])
  )
);

export { crc32, constants };
