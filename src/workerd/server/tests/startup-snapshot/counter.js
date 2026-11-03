// Counts its own evaluations, so that a test can tell whether an import evaluated it again.
globalThis.counterEvaluations = (globalThis.counterEvaluations ?? 0) + 1;

export const instance = {};
