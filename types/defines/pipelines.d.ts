// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
declare module "cloudflare:pipelines" {
  export abstract class PipelineTransformationEntrypoint<Env = unknown, I extends PipelineRecord = PipelineRecord, O extends PipelineRecord = PipelineRecord> {
    protected env: Env;
    protected ctx: ExecutionContext;
    constructor(ctx: ExecutionContext, env: Env);

    /**
     * run receives an array of PipelineRecord which can be
     * transformed and returned to the pipeline
     * @param records Incoming records from the pipeline to be transformed
     * @param metadata Information about the specific pipeline calling the transformation entrypoint
     * @returns A promise containing the transformed PipelineRecord array
     */
    public run(records: I[], metadata: PipelineBatchMetadata): Promise<O[]>;
  }
  export type PipelineRecord = Record<string, unknown>
  export type PipelineBatchMetadata = {
    pipelineId: string;
    pipelineName: string;
  }
  export interface Pipeline<T extends PipelineRecord = PipelineRecord> {
    /**
     * The Pipeline interface represents the type of a binding to a Pipeline
     *
     * @param records The records to send to the pipeline
     */
    send(records: T[]): Promise<void>
  }
}
