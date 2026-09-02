// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/*****************************
 *
 * NOTE: this is copy & pasted from the types/ folder, as when bazel
 * runs it doesn't have access to that directly and thusly is sad.
 * TODO: come up with a better system for this.
 *
 ****************************** /

/**
 * NonRetryableError allows for a user to throw a fatal error
 * that makes a Workflow instance fail immediately without triggering a retry
 */
declare abstract class NonRetryableError extends Error {
  /**
   * `__brand` is used to differentiate between `NonRetryableError` and `Error`
   * and is omitted from the constructor because users should not set it
   */
  constructor(message: string, name?: string);
}

declare abstract class Workflow<PARAMS = unknown> {
  /**
   * Get a handle to an existing instance of the Workflow.
   * @param id Id for the instance of this Workflow
   * @returns A promise that resolves with a handle for the Instance
   */
  get(id: string): Promise<WorkflowInstance>;

  /**
   * Create a new instance and return a handle to it. If a provided id exists, an error will be thrown.
   * @param options Options when creating an instance including name and params
   * @returns A promise that resolves with a handle for the Instance
   */
  create(
    options?: WorkflowInstanceCreateOptions<PARAMS>
  ): Promise<WorkflowInstance>;

  /**
   * Create a batch of instances and return handles for the created instances and any per-instance errors.
   * `createBatch` is limited at 100 instances at a time or when the RPC limit (1MiB) is reached.
   * @param options Options for creating instances by count or from a list of instance options
   * @returns A promise that resolves with the created instance handles and any per-instance errors.
   */
  createBatch(
    options: WorkflowBatchCreateOptions<PARAMS>
  ): Promise<WorkflowBatchCreateResult>;

  /**
   * Create a batch of instances and return handles for all of them.
   * @deprecated Use the object form of `createBatch` instead of the array form.
   */
  createBatch(
    batch: WorkflowInstanceCreateOptions<PARAMS>[]
  ): Promise<WorkflowInstance[]>;
}

type WorkflowBatchCreateOptions<PARAMS = unknown> =
  | {
      count: number;
      params?: PARAMS;
      retention?: {
        successRetention?: WorkflowRetentionDuration;
        errorRetention?: WorkflowRetentionDuration;
      };
      locationHint?: WorkflowInstanceLocationHint;
      instances?: never;
    }
  | {
      instances: WorkflowInstanceCreateOptions<PARAMS>[];
      count?: never;
      params?: never;
      retention?: never;
      locationHint?: never;
    };

type WorkflowBatchCreateResult = {
  created: WorkflowInstance[];
  errors: {
    index: number;
    id?: string;
    code: number;
    message: string;
  }[];
};

type WorkflowDurationLabel =
  'second' | 'minute' | 'hour' | 'day' | 'week' | 'month' | 'year';

type WorkflowSleepDuration =
  `${number} ${WorkflowDurationLabel}${'s' | ''}` | number;

type WorkflowRetentionDuration = WorkflowSleepDuration;

/** Geographic regions supported when creating a Workflow instance.
 * Location hints are best-effort placement preferences. */
type WorkflowInstanceLocationHint =
  | 'wnam'
  | 'enam'
  | 'sam'
  | 'weur'
  | 'eeur'
  | 'apac'
  | 'apac-ne'
  | 'apac-se'
  | 'oc'
  | 'afr'
  | 'me';

interface WorkflowInstanceCreateOptions<PARAMS = unknown> {
  /**
   * An id for your Workflow instance. Must be unique within the Workflow.
   * This is automatically generated if not passed in.
   */
  id?: string;
  /**
   * The event payload the Workflow instance is triggered with
   */
  params?: PARAMS;
  /**
   * The retention policy for the Workflow instance.
   * Defaults to the maximum retention period available for the owner's account.
   */
  retention?: {
    successRetention?: WorkflowRetentionDuration;
    errorRetention?: WorkflowRetentionDuration;
  };
  /** A best-effort geographic placement preference for the Workflow instance.
   * See `WorkflowInstanceLocationHint` for supported regions. */
  locationHint?: WorkflowInstanceLocationHint;
}

type InstanceStatus = {
  status:
    | 'queued' // means that instance is waiting to be started (see concurrency limits)
    | 'running'
    | 'paused'
    | 'errored'
    | 'terminated' // user terminated the instance while it was running
    | 'complete'
    | 'waiting' // instance is hibernating and waiting for sleep or event to finish
    | 'waitingForPause' // instance is finishing the current work to pause
    | 'unknown';
  error?: {
    name: string;
    message: string;
  };
  output?: unknown;
};

interface WorkflowError {
  code?: number;
  message: string;
}

interface WorkflowInstanceTerminateOptions {
  /**
   * If true, run registered rollback handlers before terminating the instance.
   * Only steps that registered rollback handlers are rolled back.
   */
  rollback?: boolean;
}

interface WorkflowInstanceRestartOptions {
  /**
   * Restart from a specific step. If omitted, the instance restarts from the beginning.
   * The step must exist in the instance's execution history.
   */
  from?: {
    /**
     * The step name as defined in your workflow code.
     */
    name: string;
    /**
     * 1-indexed occurrence of this step name. Use when the same step name appears multiple times (e.g. in a loop).
     * @default 1
     */
    count?: number;
    /**
     * Step type filter. Use when different step types share the same name.
     */
    type?: 'do' | 'sleep' | 'waitForEvent';
  };
}

type WorkflowInstanceEventCommon = {
  instanceId: string;
  eventId: number;
  timestamp: number;
};

type WorkflowInstanceEvent = WorkflowInstanceEventCommon &
  (
    | { type: 'workflow_queued' }
    | { type: 'workflow_started'; params?: unknown }
    | { type: 'workflow_completed'; output?: unknown }
    | { type: 'workflow_failed'; error: { name: string; message: string } }
    | { type: 'workflow_terminated' }
    | { type: 'step_started'; stepName: string }
    | { type: 'step_completed'; stepName: string; output?: unknown }
    | { type: 'step_failed'; stepName: string }
    | { type: 'attempt_started'; stepName: string; attempt: number }
    | { type: 'attempt_completed'; stepName: string; attempt: number }
    | {
        type: 'attempt_failed';
        stepName: string;
        attempt: number;
        retryDelayMs?: number;
        error: { name: string; message: string };
      }
    | { type: 'sleep_started'; stepName: string; durationMs: number }
    | { type: 'sleep_completed'; stepName: string }
    | { type: 'wait_started'; stepName: string; eventType: string }
    | { type: 'wait_completed'; stepName: string }
    | { type: 'wait_timed_out'; stepName: string }
    | { type: 'rollback_started' }
    | { type: 'rollback_step_started'; stepName: string }
    | { type: 'rollback_step_completed'; stepName: string }
    | {
        type: 'rollback_step_failed';
        stepName: string;
        error: { name: string; message: string };
      }
    | { type: 'rollback_attempt_started'; stepName: string; attempt: number }
    | { type: 'rollback_attempt_completed'; stepName: string; attempt: number }
    | {
        type: 'rollback_attempt_failed';
        stepName: string;
        attempt: number;
        retryDelayMs?: number;
        error: { name: string; message: string };
      }
    | { type: 'rollback_completed' }
    | { type: 'rollback_failed' }
  );

type WorkflowInstanceEventType = WorkflowInstanceEvent['type'];

type WorkflowInstanceSubscribeOptions = {
  cursor?: number;
  filter?: WorkflowInstanceEventType[];
};

interface WorkflowInstanceSubscription extends Disposable {
  next(): Promise<IteratorResult<WorkflowInstanceEvent, undefined>>;
}

declare abstract class WorkflowInstance {
  id: string;

  /**
   * Pause the instance.
   */
  pause(): Promise<void>;

  /**
   * Resume the instance. If it is already running, an error will be thrown.
   */
  resume(): Promise<void>;

  /**
   * Terminate the instance. If it is errored, terminated or complete, an error will be thrown.
   * @param options Options for termination, including whether registered rollback handlers should run.
   */
  terminate(options?: WorkflowInstanceTerminateOptions): Promise<void>;

  /**
   * Restart the instance. Optionally restart from a specific step, preserving
   * cached results for all steps before it.
   * @param options Options for the restart, including an optional step to restart from.
   */
  restart(options?: WorkflowInstanceRestartOptions): Promise<void>;

  /**
   * Returns the current status of the instance.
   */
  status(): Promise<InstanceStatus>;

  /**
   * Send an event to this instance.
   */
  sendEvent({
    type,
    payload,
  }: {
    type: string;
    payload: unknown;
  }): Promise<void>;

  subscribe(
    options?: WorkflowInstanceSubscribeOptions
  ): Promise<WorkflowInstanceSubscription>;
}
