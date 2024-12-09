import { default as dnsUtil } from 'node-internal:dns';
import * as errorCodes from 'node-internal:internal_dns_constants';
import { DnsError } from 'node-internal:internal_errors';
import { validateString } from 'node-internal:validators';

export type TTLResponse = {
  ttl: number;
  address: string;
};
export interface Answer {
  // The record owner.
  name: string;
  // The type of DNS record.
  // These are defined here: https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-4
  type: number;
  // The number of seconds the answer can be stored in cache before it is considered stale.
  TTL: number;
  // The value of the DNS record for the given name and type. The data will be in text for standardized record types and in hex for unknown types.
  data: string;
}
export interface FailedResponse {
  error: string;
}
export interface SuccessResponse {
  // The Response Code of the DNS Query.
  // These are defined here: https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-6
  Status: number;
  // If true, it means the truncated bit was set.
  // This happens when the DNS answer is larger than a single UDP or TCP packet.
  // TC will almost always be false with Cloudflare DNS over HTTPS because Cloudflare supports the maximum response size.
  TC: boolean;
  // If true, it means the Recursive Desired bit was set.
  RD: boolean;
  // If true, it means the Recursion Available bit was set.
  RA: boolean;
  // 	If true, it means that every record in the answer was verified with DNSSEC.
  AD: boolean;
  // 	If true, the client asked to disable DNSSEC validation.
  CD: boolean;
  Question: {
    // The record name requested.
    name: string;
    // The type of DNS record requested.
    // These are defined here: https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-4
    type: number;
  }[];
  Answer?: Answer[];
  Authority: Answer[];
  Additional?: Answer;
}

export async function sendDnsRequest(
  name: string,
  type: string
): Promise<SuccessResponse> {
  // We are using cloudflare-dns.com and not 1.1.1.1 because of certificate issues.
  // TODO(soon): Replace this when KJ certificate issues are resolved.
  const server = new URL('https://cloudflare-dns.com/dns-query');
  server.searchParams.set('name', name);
  server.searchParams.set('type', type);

  // syscall needs to be in format of `queryTxt`
  // eslint-disable-next-line @typescript-eslint/restrict-template-expressions
  const syscall = `query${type.at(0)?.toUpperCase()}${type.slice(1)}`;

  let json: SuccessResponse | FailedResponse;
  try {
    const response = await fetch(server, {
      headers: {
        Accept: 'application/dns-json',
      },
      method: 'GET',
    });
    // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
    json = await response.json();
  } catch {
    throw new DnsError(name, errorCodes.BADQUERY, syscall);
  }

  if ('error' in json) {
    throw new DnsError(name, errorCodes.BADRESP, syscall);
  }

  if (!json.Question.at(0)) {
    // Some APIs depend on Question being existent.
    throw new DnsError(name, errorCodes.BADRESP, syscall);
  }

  if (json.Answer?.at(0)?.name === '') {
    throw new DnsError(name, errorCodes.NOTFOUND, syscall);
  }

  return json;
}

export function validateAnswer(
  answer: unknown,
  name: string,
  query: string
): asserts answer is Answer[] {
  if (answer == null) {
    throw new DnsError(name, errorCodes.NOTFOUND, query);
  }
}

export type MX = {
  exchange: string;
  priority: number;
};
export function normalizeMx(name: string, answer: Answer): MX {
  const [priority, exchange]: string[] = answer.data.split(' ');
  if (priority == null || exchange == null) {
    throw new DnsError(name, errorCodes.BADRESP, 'queryMx');
  }

  // Cloudflare API returns "data": "10 smtp.google.com." hence
  // we need to parse it. Let's play it safe.
  if (exchange.endsWith('.')) {
    return {
      exchange: exchange.slice(0, -1),
      priority: parseInt(priority, 10),
    };
  }

  return {
    exchange,
    priority: parseInt(priority, 10),
  };
}

export function normalizeCname({ data }: Answer): string {
  // Cloudflare DNS returns "nodejs.org." whereas
  // Node.js returns "nodejs.org" as a CNAME data.
  if (data.endsWith('.')) {
    return data.slice(0, -1);
  }
  return data;
}

export type CAA = {
  critical: number;
  issue?: string;
  iodef?: string;
  issuewild?: string;
};
export function normalizeCaa({ data }: Answer): CAA {
  // CAA API returns "hex", so we need to convert it to UTF-8
  const record = dnsUtil.parseCaaRecord(data);
  const obj: CAA = { critical: record.critical };
  obj[record.field] = record.value;
  return obj;
}

export type NAPTR = {
  flags: string;
  service: string;
  regexp: string;
  replacement: string;
  order: number;
  preference: number;
};
export function normalizeNaptr({ data }: Answer): NAPTR {
  // Cloudflare DNS appends "." at the end whereas Node.js doesn't.
  return dnsUtil.parseNaptrRecord(data);
}

export function normalizePtr({ data }: Answer): string {
  if (data.endsWith('.')) {
    return data.slice(0, -1);
  }
  return data;
}

export function normalizeNs({ data }: Answer): string {
  if (data.endsWith('.')) {
    return data.slice(0, -1);
  }
  return data;
}

export type SOA = {
  nsname: string;
  hostmaster: string;
  serial: number;
  refresh: number;
  retry: number;
  expire: number;
  minttl: number;
};
export function normalizeSoa({ data }: Answer): SOA {
  // Cloudflare DNS returns ""meera.ns.cloudflare.com. dns.cloudflare.com. 2357999196 10000 2400 604800 1800""
  const [nsname, hostmaster, serial, refresh, retry, expire, minttl] =
    data.split(' ');

  validateString(nsname, 'nsname');
  validateString(hostmaster, 'hostmaster');
  validateString(serial, 'serial');
  validateString(refresh, 'refresh');
  validateString(retry, 'retry');
  validateString(expire, 'expire');
  validateString(minttl, 'minttl');

  return {
    nsname,
    hostmaster,
    serial: parseInt(serial, 10),
    refresh: parseInt(refresh, 10),
    retry: parseInt(retry, 10),
    expire: parseInt(expire, 10),
    minttl: parseInt(minttl, 10),
  };
}

export type SRV = {
  name: string;
  port: number;
  priority: number;
  weight: number;
};
export function normalizeSrv({ data }: Answer): SRV {
  // Cloudflare DNS returns "5 0 80 calendar.google.com"
  const [priority, weight, port, name] = data.split(' ');
  validateString(priority, 'priority');
  validateString(weight, 'weight');
  validateString(port, 'port');
  validateString(name, 'name');
  return {
    priority: parseInt(priority, 10),
    weight: parseInt(weight, 10),
    port: parseInt(port, 10),
    name,
  };
}

export function normalizeTxt({ data }: Answer): string[] {
  // Each entry has quotation marks as a prefix and suffix.
  // Node.js APIs doesn't have them.
  if (data.startsWith('"') && data.endsWith('"')) {
    return [data.slice(1, -1)];
  }
  return [data];
}
