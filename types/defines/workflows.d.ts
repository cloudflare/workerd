declare module "cloudflare:workflows" {
  /**
   * NonRetryableError allows for a user to throw a fatal error
   * that makes a Workflow instance fail immediately without triggering a retry
   */
  export class NonRetryableError extends Error {
    public constructor(message: string, name?: string);
  }
}

declare abstract class Workflow {
  /**
   * Get a handle to an existing instance of the Workflow.
   * @param id Id for the instance of this Workflow
   * @returns A promise that resolves with a handle for the Instance
   */
  public get(id: string): Promise<Instance>;

  /**
   * Create a new instance and return a handle to it. If a provided id exists, an error will be thrown.
   * @param options optional fields to customize the instance creation
   * @returns A promise that resolves with a handle for the Instance
   */
  public create(options?: WorkflowInstanceCreateOptions): Promise<Instance>;
}

interface WorkflowInstanceCreateOptions {
  /**
   * Name to create the instance of this Workflow with - it should always be unique
   */
  name?: string;
  /**
   * The payload to send over to this instance, this is optional since you might need to pass params into the instance
   */
  params?: unknown;
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

declare abstract class Instance {
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
   * Abort the instance. If it is errored, terminated or complete, an error will be thrown.
   */
  public abort(): Promise<void>;

  /**
   * Restart the instance.
   */
  public restart(): Promise<void>;

  /**
   * Returns the current status of the instance.
   */
  public status(): Promise<InstanceStatus>;
}
