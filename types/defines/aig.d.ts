export type GatewayOptions = {
  id: string;
  cacheKey?: string;
  cacheTtl?: number;
  skipCache?: boolean;
  metadata?: Record<string, number | string | boolean | null | bigint>;
  collectLog?: boolean;
};

export type AiGatewayPatchLog = {
  score?: number | null;
  feedback?: -1 | 1 | null;
  metadata?: Record<string, number | string | boolean | null | bigint> | null;
};

export type AiGatewayLog = {
  id: string;
  provider: string;
  model: string;
  model_type?: string;
  path: string;
  duration: number;
  request_type?: string;
  request_content_type?: string;
  status_code: number;
  response_content_type?: string;
  success: boolean;
  cached: boolean;
  tokens_in?: number;
  tokens_out?: number;
  metadata?: Record<string, number | string | boolean | null | bigint>;
  step?: number;
  cost?: number;
  custom_cost?: boolean;
  request_size: number;
  request_head?: string;
  request_head_complete: boolean;
  response_size: number;
  response_head?: string;
  response_head_complete: boolean;
  created_at: Date;
};

export type AIGatewayProviders =
  | 'workers-ai'
  | 'anthropic'
  | 'aws-bedrock'
  | 'azure-openai'
  | 'google-vertex-ai'
  | 'huggingface'
  | 'openai'
  | 'perplexity-ai'
  | 'replicate'
  | 'groq'
  | 'cohere'
  | 'google-ai-studio'
  | 'mistral'
  | 'grok'
  | 'openrouter';

export type AIGatewayHeaders = {
  'cf-aig-metadata':
    | Record<string, number | string | boolean | null | bigint>
    | string;
  'cf-aig-custom-cost':
    | { per_token_in?: number; per_token_out?: number }
    | { total_cost?: number }
    | string;
  'cf-aig-cache-ttl': number | string;
  'cf-aig-skip-cache': boolean | string;
  'cf-aig-cache-key': string;
  'cf-aig-collect-log': boolean | string;
  Authorization: string;
  'Content-Type': string;
  [key: string]: string | number | boolean | object;
};

export type AIGatewayUniversalRequest = {
  provider: AIGatewayProviders | string; // eslint-disable-line
  endpoint: string;
  headers: Partial<AIGatewayHeaders>;
  query: unknown;
};

export interface AiGatewayInternalError extends Error {}
export interface AiGatewayLogNotFound extends Error {}

export declare abstract class AiGateway {
  patchLog(logId: string, data: AiGatewayPatchLog): Promise<void>;
  getLog(logId: string): Promise<AiGatewayLog>;
  run(data: AIGatewayUniversalRequest | AIGatewayUniversalRequest[]): Promise<Response>;
}
