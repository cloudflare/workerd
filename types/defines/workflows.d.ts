declare module "cloudflare:workflows" {
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
   * Create a batch of instances and return handle for all of them. If a provided id exists, an error will be thrown.
   * `createBatch` is limited at 100 instances at a time or when the RPC limit for the batch (1MiB) is reached.
   * @param batch List of Options when creating an instance including name and params
   * @returns A promise that resolves with a list of handles for the created instances.
   */
  public createBatch(
    batch: WorkflowInstanceCreateOptions<PARAMS>[]
  ): Promise<WorkflowInstance[]>;
}

interface WorkflowInstanceCreateOptions<PARAMS = unknown> {
  /**
   * An id for your Workflow instance. Must be unique within the Workflow.
   */
  id?: string;
  /**
   * The event payload the Workflow instance is triggered with
   */
  params?: PARAMS;
}

type InstanceStatus = {
  status:
    | "queued" // means that instance is waiting to be started (see concurrency limits)
    | "running"
    | "paused"
    | "errored"
    | "terminated" // user terminated the instance while it was running
    | "complete"
    | "waiting" // instance is hibernating and waiting for sleep or event to finish
    | "waitingForPause" // instance is finishing the current work to pause
    | "unknown";
  error?: string;
  output?: object;
};

interface WorkflowError {
  code?: number;
  message: string;
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
   */
  public terminate(): Promise<void>;

  /**
   * Restart the instance.
   */
  public restart(): Promise<void>;

  /**
   * Returns the current status of the instance.
   */
  public status(): Promise<InstanceStatus>;
}
