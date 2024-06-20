
interface FS {
  mkdir: Function,
  writeFile: Function
}

interface Module {
  HEAP8: Uint8Array,
  _dump_traceback: Function,
  FS: FS,
}
