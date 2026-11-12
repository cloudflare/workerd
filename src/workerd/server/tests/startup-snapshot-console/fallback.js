// An AbortController at module scope has no snapshotClone(), so the snapshot fails and the
// Worker evaluates this module a second time.
globalThis.controller = new AbortController();

console.log('MARK fallback log');
console.warn('MARK fallback warn');

export default {
  async test() {},
};
