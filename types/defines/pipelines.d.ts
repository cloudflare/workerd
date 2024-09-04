// Copyright (c) 2022-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export interface Pipeline {
  /**
   * send takes an array of javascript objects which are
   * then received by the pipeline for processing
   *
   * @param data The data to be sent
   */
  send(data: object[]): Promise<void>
}
