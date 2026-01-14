// internal/types.ts
function isString(value) {
  return typeof value === "string";
}
function isArrayBuffer(value) {
  return value instanceof ArrayBuffer;
}
function isUint8Array(value) {
  return value instanceof Uint8Array;
}
export {
  isArrayBuffer,
  isString,
  isUint8Array
};
