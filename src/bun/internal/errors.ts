// Error classes for Bun runtime

export class ERR_FS_FILE_NOT_FOUND extends Error {
  code = 'ENOENT'
  constructor(path: string) {
    super(`ENOENT: no such file or directory, open '${path}'`)
    this.name = 'ERR_FS_FILE_NOT_FOUND'
  }
}
