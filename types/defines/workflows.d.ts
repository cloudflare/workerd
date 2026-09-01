declare module 'cloudflare:workflows' {
  /**
   * NonRetryableError allows for a user to throw a fatal error
   * that makes a Workflow instance fail immediately without triggering a retry
   */
  export class NonRetryableError extends Error {
    public constructor(message: string, name?: string);
  }
}

declare abstract class Workflow<PARAMS = unknown> {
  /**
   * Get a handle to an existing instance of the Workflow.
   * @param id Id for the instance of this Workflow
   * @returns A promise that resolves with a handle for the Instance
   */
  public get(id: string): Promise<WorkflowInstance>;

  /**
   * Create a new instance and return a handle to it. If a provided id exists, an error will be thrown.
   * @param options Options when creating an instance including id and params
   * @returns A promise that resolves with a handle for the Instance
   */
  public create(
    options?: WorkflowInstanceCreateOptions<PARAMS>
  ): Promise<WorkflowInstance>;

  /**
   * Create a batch of instances and return handles for the created instances and any per-instance errors.
   * `createBatch` is limited at 100 instances at a time or when the RPC limit for the batch (1MiB) is reached.
   * @param options Options for creating instances by count or from a list of instance options
   * @returns A promise that resolves with the created instance handles and any per-instance errors.
   */
  public createBatch(
    options: WorkflowBatchCreateOptions<PARAMS>
  ): Promise<WorkflowBatchCreateResult>;

  /**
   * Create a batch of instances and return handles for all of them.
   * @deprecated Use the object form `createBatch({ instances: batch })` instead.
   */
  public createBatch(
    batch: WorkflowInstanceCreateOptions<PARAMS>[]
  ): Promise<WorkflowInstance[]>;

  /**
   * Delete a batch of Workflow instances and their stored state.
   * `deleteBatch` is limited to 100 instances at a time. Duplicate IDs are deleted once.
   * The result contains one entry for each input position; IDs that do not exist are returned as per-instance errors.
   * @param instanceIds IDs of the Workflow instances to delete
   * @returns A promise that resolves with the successfully deleted instances and any per-instance errors.
   */
  public deleteBatch(instanceIds: string[]): Promise<WorkflowBatchDeleteResult>;
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

type WorkflowBatchDeleteResult = {
  deleted: { id: string }[];
  errors: {
    id: string;
    code: number;
    message: string;
  }[];
};

type WorkflowDurationLabel =
  | 'second'
  | 'minute'
  | 'hour'
  | 'day'
  | 'week'
  | 'month'
  | 'year';

type WorkflowSleepDuration =
  | `${number} ${WorkflowDurationLabel}${'s' | ''}`
  | number;

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
   */
  id?: string;
  /**
   * The event payload the Workflow instance is triggered with
   */
  params?: PARAMS;
  /**
   * The retention policy for Workflow instance.
   * Defaults to the maximum retention period available for the owner's account.
   */
  retention?: {
    successRetention?: WorkflowRetentionDuration,
    errorRetention?: WorkflowRetentionDuration,
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

declare abstract class WorkflowInstance {
  public id: string;

  /**
   * Pause the instance.
   */
  public pause(): Promise<void>;

  /**
   * Resume the instance. If it is already running, an error will be thrown.
   */
  public resume(): Promise<void>;

  /**
   * Terminate the instance. If it is errored, terminated or complete, an error will be thrown.
   * @param options Options for termination, including whether registered rollback handlers should run.
   */
  public terminate(options?: WorkflowInstanceTerminateOptions): Promise<void>;

  /**
   * Restart the instance. Optionally restart from a specific step, preserving
   * cached results for all steps before it.
   * @param options Options for the restart, including an optional step to restart from.
   */
  public restart(options?: WorkflowInstanceRestartOptions): Promise<void>;

  /**
   * Delete the instance and its stored state.
   */
  public delete(): Promise<void>;

  /**
   * Returns the current status of the instance.
   */
  public status(): Promise<InstanceStatus>;

  /**
   * Send an event to this instance.
   */
  public sendEvent({
    type,
    payload,
  }: {
    type: string;
    payload: unknown;
  }): Promise<void>;
}
