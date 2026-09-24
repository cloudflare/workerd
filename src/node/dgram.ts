// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { Socket, createSocket } from 'node-internal:internal_dgram';

export { Socket, createSocket };

export default {
  createSocket,
  Socket,
};
