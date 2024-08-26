// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export abstract class PipelineTransform {
  /**
   * transformJson recieves an array of javascript objects which can be
   * mutated and returned to the pipeline
   * @param data The data to be mutated
   * @returns A promise containing the mutated data
   */
  public transformJson(data: object[]): Promise<object[]>;
}
