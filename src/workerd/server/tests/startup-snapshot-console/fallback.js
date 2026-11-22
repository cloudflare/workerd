// An aborted AbortSignal at module scope holds its reason, a JavaScript value no snapshotClone()
// carries, so the snapshot fails and the Worker evaluates this module a second time.
globalThis.signal = AbortSignal.abort('fallback');

console.log('MARK fallback log');
console.warn('MARK fallback warn');

export default {
  async test() {},
};
