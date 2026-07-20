// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export class NonRetryableError extends Error {
  constructor(message: string, name = 'NonRetryableError') {
    super(message);
    this.name = name;
  }
}

interface Fetcher {
  getInstance(id: string): Promise<{ id: string }>;
  create(options?: WorkflowInstanceCreateOptions): Promise<{ id: string }>;
  createBatch(
    options: WorkflowInstanceCreateOptions[]
  ): Promise<{ id: string }[]>;
  pause(id: string): Promise<void>;
  resume(id: string): Promise<void>;
  terminate(
    id: string,
    options?: WorkflowInstanceTerminateOptions
  ): Promise<void>;
  restart(id: string, options?: WorkflowInstanceRestartOptions): Promise<void>;
  status(id: string): Promise<InstanceStatus>;
  sendEvent(
    id: string,
    event: { type: string; payload: unknown }
  ): Promise<void>;
}

class InstanceImpl implements WorkflowInstance {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private readonly fetcher: Fetcher;
  readonly id: string;

  constructor(id: string, fetcher: Fetcher) {
    this.id = id;
    this.fetcher = fetcher;
  }

  async pause(): Promise<void> {
    await this.fetcher.pause(this.id);
  }
  async resume(): Promise<void> {
    await this.fetcher.resume(this.id);
  }

  async terminate(options?: WorkflowInstanceTerminateOptions): Promise<void> {
    await this.fetcher.terminate(this.id, options);
  }

  async restart(options?: WorkflowInstanceRestartOptions): Promise<void> {
    await this.fetcher.restart(this.id, options);
  }

  async status(): Promise<InstanceStatus> {
    return await this.fetcher.status(this.id);
  }

  async sendEvent({
    type,
    payload,
  }: {
    type: string;
    payload: unknown;
  }): Promise<void> {
    await this.fetcher.sendEvent(this.id, { type, payload });
  }
}

class WorkflowImpl {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private readonly fetcher: Fetcher;

  constructor(fetcher: Fetcher) {
    this.fetcher = fetcher;
  }

  async get(id: string): Promise<WorkflowInstance> {
    const result = await this.fetcher.getInstance(id);
    return new InstanceImpl(result.id, this.fetcher);
  }

  async create(
    options?: WorkflowInstanceCreateOptions
  ): Promise<WorkflowInstance> {
    const result = await this.fetcher.create(options);
    return new InstanceImpl(result.id, this.fetcher);
  }

  async createBatch(
    options: WorkflowInstanceCreateOptions[]
  ): Promise<WorkflowInstance[]> {
    const results = await this.fetcher.createBatch(options);
    return results.map((result) => new InstanceImpl(result.id, this.fetcher));
  }
}

export function makeBinding(env: { fetcher: Fetcher }): Workflow {
  return new WorkflowImpl(env.fetcher);
}

export default makeBinding;
