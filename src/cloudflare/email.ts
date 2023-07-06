// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TODO: c++ built-ins do not yet support named exports
import { default as email } from 'cloudflare-internal:email';
export const { EmailMessage } = email;
