// ffi.ts
function notAvailable(name) {
  throw new Error(
    `bun:ffi ${name}() is not available in workerd. FFI requires native code execution.`
  );
}
function dlopen(_path, _symbols) {
  notAvailable("dlopen");
}
function CString(_ptr) {
  notAvailable("CString");
}
function ptr(_data) {
  notAvailable("ptr");
}
function read(_ptr, _type) {
  notAvailable("read");
}
function write(_ptr, _value, _type) {
  notAvailable("write");
}
function allocate(_size) {
  notAvailable("allocate");
}
function free(_ptr) {
  notAvailable("free");
}
function callback(_fn, _definition) {
  notAvailable("callback");
}
function toArrayBuffer(_ptr, _byteLength) {
  notAvailable("toArrayBuffer");
}
function viewSource(_ptr, _type, _length) {
  notAvailable("viewSource");
}
var FFI_TYPES = {
  void: 0,
  bool: 1,
  char: 2,
  i8: 3,
  i16: 4,
  i32: 5,
  i64: 6,
  u8: 7,
  u16: 8,
  u32: 9,
  u64: 10,
  f32: 11,
  f64: 12,
  pointer: 13,
  cstring: 14
};
var ffi_default = {
  dlopen,
  CString,
  ptr,
  read,
  write,
  allocate,
  free,
  callback,
  toArrayBuffer,
  viewSource,
  FFI_TYPES
};
export {
  CString,
  FFI_TYPES,
  allocate,
  callback,
  ffi_default as default,
  dlopen,
  free,
  ptr,
  read,
  toArrayBuffer,
  viewSource,
  write
};
