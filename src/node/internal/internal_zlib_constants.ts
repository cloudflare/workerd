import { default as zlibUtil } from 'node-internal:zlib';

const {
  CONST_Z_OK,
  CONST_Z_STREAM_END,
  CONST_Z_NEED_DICT,
  CONST_Z_ERRNO,
  CONST_Z_STREAM_ERROR,
  CONST_Z_DATA_ERROR,
  CONST_Z_MEM_ERROR,
  CONST_Z_BUF_ERROR,
  CONST_Z_VERSION_ERROR,
} = zlibUtil;

const constPrefix = 'CONST_';
export const constants: Record<string, number> = {};

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

// Translation table for return codes.
const rawCodes: Record<string, number | string> = {
  Z_OK: CONST_Z_OK,
  Z_STREAM_END: CONST_Z_STREAM_END,
  Z_NEED_DICT: CONST_Z_NEED_DICT,
  Z_ERRNO: CONST_Z_ERRNO,
  Z_STREAM_ERROR: CONST_Z_STREAM_ERROR,
  Z_DATA_ERROR: CONST_Z_DATA_ERROR,
  Z_MEM_ERROR: CONST_Z_MEM_ERROR,
  Z_BUF_ERROR: CONST_Z_BUF_ERROR,
  Z_VERSION_ERROR: CONST_Z_VERSION_ERROR,
};

for (const key of Object.keys(rawCodes)) {
  rawCodes[rawCodes[key] as number] = key;
}

export const codes = Object.freeze(rawCodes);
