import * as _promises from 'node-internal:internal_timers_promises';
import {
  setTimeout,
  clearTimeout,
  setImmediate,
  clearImmediate,
  setInterval,
  clearInterval,
  active,
  unenroll,
  enroll,
} from 'node-internal:internal_timers';

export * from 'node-internal:internal_timers';
export const promises = _promises;

export default {
  promises: _promises,
  setTimeout,
  clearTimeout,
  setImmediate,
  clearImmediate,
  setInterval,
  clearInterval,
  active,
  unenroll,
  enroll,
};
