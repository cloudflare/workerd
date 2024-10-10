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
  public get(id: string): Promise<Instance>;

  /**
   * Create a new instance and return a handle to it. If a provided id exists, an error will be thrown.
   * @param id Id to create the instance of this Workflow with
   * @param params The payload to send over to this instance
   * @returns A promise that resolves with a handle for the Instance
   */
  public create(id: string, params: object): Promise<Instance>;
}

type InstanceStatus = {
  status:
    | 'queued'
    | 'running'
    | 'paused'
    | 'errored'
    | 'terminated'
    | 'complete'
    | 'unknown';
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
