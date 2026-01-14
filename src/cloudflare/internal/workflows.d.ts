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
  constructor(message: string, name?: string)
}

declare abstract class Workflow<PARAMS = unknown> {
  /**
   * Get a handle to an existing instance of the Workflow.
   * @param id Id for the instance of this Workflow
   * @returns A promise that resolves with a handle for the Instance
   */
  get(id: string): Promise<WorkflowInstance>

  /**
   * Create a new instance and return a handle to it. If a provided id exists, an error will be thrown.
   * @param options Options when creating an instance including name and params
   * @returns A promise that resolves with a handle for the Instance
   */
  create(
    options?: WorkflowInstanceCreateOptions<PARAMS>,
  ): Promise<WorkflowInstance>

  /**
   * Create a batch of instances and return handle for all of them. If a provided id exists, an error will be thrown.
   * `createBatch` is limited at 100 instances at a time or when the RPC limit (1MiB) is reached.
   * @param batch List of Options when creating an instance including name and params
   * @returns A promise that resolves with a list of handles for the created instances.
   */
  createBatch(
    batch: WorkflowInstanceCreateOptions<PARAMS>[],
  ): Promise<WorkflowInstance[]>
}

type WorkflowDurationLabel =
  | 'second'
  | 'minute'
  | 'hour'
  | 'day'
  | 'week'
  | 'month'
  | 'year'

type WorkflowSleepDuration =
  | `${number} ${WorkflowDurationLabel}${'s' | ''}`
  | number

type WorkflowRetentionDuration = WorkflowSleepDuration

interface WorkflowInstanceCreateOptions<PARAMS = unknown> {
  /**
   * An id for your Workflow instance. Must be unique within the Workflow.
   * This is automatically generated if not passed in.
   */
  id?: string
  /**
   * The event payload the Workflow instance is triggered with
   */
  params?: PARAMS
  /**
   * The retention policy for the Workflow instance.
   * Defaults to the maximum retention period available for the owner's account.
   */
  retention?: {
    successRetention?: WorkflowRetentionDuration
    errorRetention?: WorkflowRetentionDuration
  }
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
    | 'unknown'
  error?: {
    name: string
    message: string
  }
  output?: unknown
}

interface WorkflowError {
  code?: number
  message: string
}

declare abstract class WorkflowInstance {
  id: string

  /**
   * Pause the instance.
   */
  pause(): Promise<void>

  /**
   * Resume the instance. If it is already running, an error will be thrown.
   */
  resume(): Promise<void>

  /**
   * Terminate the instance. If it is errored, terminated or complete, an error will be thrown.
   */
  terminate(): Promise<void>

  /**
   * Restart the instance.
   */
  restart(): Promise<void>

  /**
   * Returns the current status of the instance.
   */
  status(): Promise<InstanceStatus>

  /**
   * Send an event to this instance.
   */
  sendEvent({
    type,
    payload,
  }: {
    type: string
    payload: unknown
  }): Promise<void>
}
