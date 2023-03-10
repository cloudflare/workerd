// Our implementation of process.nextTick is just queueMicrotask. The timing
// of when the queue is drained is different, as is the error handling so this
// is only an approximation of process.nextTick semantics. Hopefully it's good
// enough because we really do not want to implement Node.js' idea of a nextTick
// queue.
/* eslint-disable */

export function nextTick(cb: Function, ...args: unknown[]) {
  queueMicrotask(() => { cb(...args); });
};

export default {
  nextTick,
};
