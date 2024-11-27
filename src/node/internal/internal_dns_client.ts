import * as errorCodes from 'node-internal:internal_dns_constants';
import { DnsError } from 'node-internal:internal_errors';

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
