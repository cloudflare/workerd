interface Module {
  HEAP8: Uint8Array;
  _dump_traceback: () => void;
  FS: FS;
  _py_version_major: () => number;
  _py_version_minor: () => number;
}
