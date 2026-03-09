// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export function parseCaaRecord(record: string): {
  critical: number;
  field: 'issue' | 'iodef' | 'issuewild';
  value: string;
};

export function parseNaptrRecord(record: string): {
  flags: string;
  service: string;
  regexp: string;
  replacement: string;
  order: number;
  preference: number;
};
