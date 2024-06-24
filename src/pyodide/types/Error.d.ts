interface ErrorConstructor {
  prepareStackTrace: ((_error: Error, stack: StackItem[]) => void) | undefined;
  stackTraceLimit: number;
}

interface StackItem {
  getFunctionName: () => string;
  getFileName: () => string;
}
