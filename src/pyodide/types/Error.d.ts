interface ErrorConstructor {
  prepareStackTrace: ((_error: Error, stack: StackItem[]) => void) | undefined;
}

interface StackItem {
  getFunctionName: () => string;
  getFileName: () => string;
}
