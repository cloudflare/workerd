declare namespace internalJaeger {
  const traceId: number | null,
    enterSpan: <T>(name: string, callback: () => T) => T;
}

export default internalJaeger;
