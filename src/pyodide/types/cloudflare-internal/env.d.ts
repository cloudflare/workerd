const innerEnv: {
  pythonPatchEnv(patch: unknown): {
    [Symbol.dispose](): void
  }
}
export default innerEnv
