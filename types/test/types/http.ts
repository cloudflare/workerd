// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

const follow: RequestInit = {
  redirect: "follow",
};

const manual: RequestInit = {
  redirect: "manual",
};

const error: RequestInit = {
  // @ts-expect-error: "error" is not supported by workerd.
  redirect: "error",
};

const invalid: RequestInit = {
  // @ts-expect-error: arbitrary redirect modes are not supported.
  redirect: "invalid",
};

void follow;
void manual;
void error;
void invalid;
