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
  public constructor(message: string, name?: string);
}

declare abstract class Workflow {
  /**
   * Get a handle to an existing instance of the Workflow.
   * @param id Id for the instance of this Workflow
   * @returns A promise that resolves with a handle for the Instance
   */
  public get(id: string): Promise<WorkflowInstance>;

  /**
   * Create a new instance and return a handle to it. If a provided id exists, an error will be thrown.
   * @param options Options when creating an instance including name and params
   * @returns A promise that resolves with a handle for the Instance
   */
  public create(
    options?: WorkflowInstanceCreateOptions
  ): Promise<WorkflowInstance>;
}

interface WorkflowInstanceCreateOptions {
  /**
   * An id for your Workflow instance. Must be unique within the Workflow.
   * This is automatically generated if not passed in.
   */
  id?: string;
  /**
   * The event payload the Workflow instance is triggered with
   */
  params?: unknown;
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
