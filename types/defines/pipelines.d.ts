// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { PipelineRecord } from "./pipeline-transform";

export interface Pipeline<T> {
  /**
   * The Pipeline interface represents the type of a binding to a Pipeline
   *
   * @param records The records to send to the pipeline
   */
  send(records: PipelineRecord<T>[]): Promise<void>
}
