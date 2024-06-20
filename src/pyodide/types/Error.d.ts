
interface ErrorConstructor {
  prepareStackTrace: Function | undefined
}

interface StackItem {
  getFunctionName: () => string,
  getFileName: () => string,
}
