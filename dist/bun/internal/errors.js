// internal/errors.ts
var ERR_FS_FILE_NOT_FOUND = class extends Error {
  code = "ENOENT";
  constructor(path) {
    super(`ENOENT: no such file or directory, open '${path}'`);
    this.name = "ERR_FS_FILE_NOT_FOUND";
  }
};
export {
  ERR_FS_FILE_NOT_FOUND
};
