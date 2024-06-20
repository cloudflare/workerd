declare namespace internalJaeger {
  const traceId: number | null,
    enterSpan: (name: String, callback: () => any) => any;
}

export default internalJaeger;
