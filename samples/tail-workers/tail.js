// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  respond(name) {
    console.log('log from', name);
    return 'response from ' + name;
  },
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tail(traces) {
    console.log(traces[0].logs);
  },
  tailStream(...args) {
    console.log('tail in worker b');
    console.log(...args);
    return (...args) => {
      console.log(...args);
    };
  },
};
