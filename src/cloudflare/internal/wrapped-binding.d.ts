// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare class _WrappedBinding {
  constructor(inner: object);
}

declare const wrappedBinding: {
  WrappedBinding: typeof _WrappedBinding;
};

export default wrappedBinding;
