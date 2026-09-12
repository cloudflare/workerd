// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import wrappedBinding from 'cloudflare-internal:wrapped-binding';

export class NonRetryableError extends Error {
  constructor(message: string, name = 'NonRetryableError') {
    super(message);
    this.name = name;
  }
}

type WorkflowBatchDeleteResult = {
  deleted: { id: string }[];
  errors: { id: string; code: number; message: string }[];
};

interface Fetcher {
  getInstance(id: string): Promise<{ id: string }>;
  deleteInstance(id: string): Promise<void>;
  create(options?: WorkflowInstanceCreateOptions): Promise<{ id: string }>;
  createBatch(
    options: WorkflowInstanceCreateOptions[]
  ): Promise<{ id: string }[]>;
  createBatch(options: WorkflowBatchCreateOptions): Promise<{
    created: { id: string }[];
    errors: WorkflowBatchCreateResult['errors'];
  }>;
  deleteBatch(options: {
    instances: string[];
  }): Promise<WorkflowBatchDeleteResult>;

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
  subscribe(
    id: string,
    options?: WorkflowInstanceSubscribeOptions
  ): Promise<WorkflowInstanceSubscription>;
}

class InstanceImpl implements WorkflowInstance {
  readonly #fetcher: Fetcher;
  readonly id: string;

  constructor(id: string, fetcher: Fetcher) {
    this.id = id;
    this.#fetcher = fetcher;
  }

  async pause(): Promise<void> {
    await this.#fetcher.pause(this.id);
  }

  async resume(): Promise<void> {
    await this.#fetcher.resume(this.id);
  }

  async terminate(options?: WorkflowInstanceTerminateOptions): Promise<void> {
    await this.#fetcher.terminate(this.id, options);
  }

  async restart(options?: WorkflowInstanceRestartOptions): Promise<void> {
    await this.#fetcher.restart(this.id, options);
  }

  async delete(): Promise<void> {
    // deleteInstance, not delete: avoids colliding with the built-in Fetcher.delete(url), which
    // is still present on compatibility dates before `fetcher_no_get_put_delete`.
    await this.#fetcher.deleteInstance(this.id);
  }

  async status(): Promise<InstanceStatus> {
    return await this.#fetcher.status(this.id);
  }

  async sendEvent({
    type,
    payload,
  }: {
    type: string;
    payload: unknown;
  }): Promise<void> {
    await this.#fetcher.sendEvent(this.id, { type, payload });
  }

  async subscribe(
    options?: WorkflowInstanceSubscribeOptions
  ): Promise<WorkflowInstanceSubscription> {
    return await this.#fetcher.subscribe(this.id, options);
  }
}

class WorkflowImpl extends wrappedBinding.WrappedBinding {
  readonly #fetcher: Fetcher;

  constructor(fetcher: Fetcher) {
    super(fetcher);
    this.#fetcher = fetcher;
  }

  async get(id: string): Promise<WorkflowInstance> {
    // getInstance, not get: avoids colliding with the built-in Fetcher.get(url), which is still
    // present on compatibility dates before `fetcher_no_get_put_delete`.
    const result = await this.#fetcher.getInstance(id);

    return new InstanceImpl(result.id, this.#fetcher);
  }

  async create(
    options?: WorkflowInstanceCreateOptions
  ): Promise<WorkflowInstance> {
    const result = await this.#fetcher.create(options);

    return new InstanceImpl(result.id, this.#fetcher);
  }

  async createBatch(
    options: WorkflowInstanceCreateOptions[]
  ): Promise<WorkflowInstance[]>;
  async createBatch(
    options: WorkflowBatchCreateOptions
  ): Promise<WorkflowBatchCreateResult>;
  async createBatch(
    options: WorkflowInstanceCreateOptions[] | WorkflowBatchCreateOptions
  ): Promise<WorkflowInstance[] | WorkflowBatchCreateResult> {
    if (Array.isArray(options)) {
      const results = await this.#fetcher.createBatch(options);

      return results.map(
        (result) => new InstanceImpl(result.id, this.#fetcher)
      );
    }

    const result = await this.#fetcher.createBatch(options);

    return {
      created: result.created.map(
        ({ id }) => new InstanceImpl(id, this.#fetcher)
      ),
      errors: result.errors,
    };
  }

  async deleteBatch(instanceIds: string[]): Promise<WorkflowBatchDeleteResult> {
    return await this.#fetcher.deleteBatch({ instances: instanceIds });
  }
}

export function makeBinding(env: { fetcher: Fetcher }): Workflow {
  return new WorkflowImpl(env.fetcher);
}

export default makeBinding;
