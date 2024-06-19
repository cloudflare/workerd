
declare namespace internalJaeger {
  const traceId: number | null,
  enterSpan: (name: String, callback: Function) => void
}

export default internalJaeger;
