// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export type PipelineRecord<T> = {
  body: T;
  readonly metadata: object;
};

export type PipelineBatchMetadata = {
  pipelineId: string;
  pipelineName: string;
};

export abstract class PipelineTransformationEntrypoint<I, O> {
  /**
   * run recieves an array of PipelineRecord which can be
   * mutated and returned to the pipeline
   * @param records Incoming records from the pipeline to be transformed
   * @param metadata Information about the specific pipeline calling the transformation entrypoint
   * @returns A promise containing the transformed PipelineRecord array
   */
  public run(records: PipelineRecord<I>[], metadata: PipelineBatchMetadata): Promise<PipelineRecord<O>[]>;
}
