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
