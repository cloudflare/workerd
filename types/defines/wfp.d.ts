// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// https://developers.cloudflare.com/cloudflare-for-platforms/workers-for-platforms/


interface DynamicDispatchLimits {
  /**
   * Limit CPU time in milliseconds.
   */
  cpuMs?: number;
  /**
   * Limit number of subrequests.
   */
  subRequests?: number;
}

interface DynamicDispatchOptions {
  /**
   * Limit resources of invoked Worker script.
   */
  limits?: DynamicDispatchLimits
  /**
   * Arguments for outbound Worker script, if configured.
   */
  outbound?: {[key: string]: any}
}

interface DispatchNamespace {
  /**
  * @param name Name of the Worker script.
  * @param args Arguments to Worker script.
  * @param options Options for Dynamic Dispatch invocation.
  * @returns A Fetcher object that allows you to send requests to the Worker script.
  * @throws If the Worker script does not exist in this dispatch namespace, an error will be thrown.
  */
  get(name: string, args?: {[key: string]: any}, options?: DynamicDispatchOptions ): Fetcher;
}
