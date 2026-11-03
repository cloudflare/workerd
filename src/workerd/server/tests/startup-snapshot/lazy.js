// Not imported at the top level: only a test's dynamic import loads it, in the Worker started
// from the snapshot.
export { instance as counterInstance } from './counter.js';
