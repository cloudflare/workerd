// Survey config.
import { type TestRunnerConfig } from 'harness/harness';

export default {
  "post-task-abort-reason.any.js": {},
  "post-task-delay.any.js": {},
  "post-task-multiple-scheduler-order.window.js": {},
  "post-task-result-success.any.js": {},
  "post-task-result-throws.any.js": {},
  "post-task-run-order.any.js": {},
  "post-task-with-abort-signal-in-handler.any.js": {},
  "post-task-with-abort-signal.any.js": {},
  "post-task-with-aborted-signal.any.js": {},
  "post-task-with-signal-and-priority.any.js": {},
  "post-task-without-signals.any.js": {},
  "scheduler-replaceable.any.js": {},
  "task-controller-abort-completed-tasks.any.js": {},
  "task-controller-abort-signal-and-priority.any.js": {},
  "task-controller-abort1.any.js": {},
  "task-controller-abort2.any.js": {},
  "task-controller-setPriority-delayed-task.any.js": {},
  "task-controller-setPriority-recursive.any.js": {},
  "task-controller-setPriority-repeated.any.js": {},
  "task-controller-setPriority1.any.js": {},
  "task-controller-setPriority2.any.js": {},
  "task-signal-onprioritychange.any.js": {}
} satisfies TestRunnerConfig;
