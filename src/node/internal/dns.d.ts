export function parseCaaRecord(record: string): {
  critical: number;
  field: string;
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
