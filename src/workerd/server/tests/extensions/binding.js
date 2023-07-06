// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export function wrap(env) {
  if (!env.secret) {
    throw new Error("secret internal binding is not specified");
  }

  return {
    tryOpen(key) {
      return key === env.secret;
    }
  }
}


export default wrap;
