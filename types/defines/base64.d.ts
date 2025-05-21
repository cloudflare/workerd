declare module "cloudflare:base64" {
  function _encodeArray(input: ArrayBufferLike | ArrayBufferView): ArrayBuffer;
  function _encodeArrayToString(input: ArrayBufferLike | ArrayBufferView): string ;
  function _decodeArray(input: ArrayBufferLike | ArrayBufferView): ArrayBuffer;
  export {
    _encodeArray as encodeArray,
    _encodeArrayToString as encodeArrayToString,
    _decodeArray as decodeArray
  };
}
