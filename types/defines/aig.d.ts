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
  feedback?: -1 | 1 | "-1" | "1" | null;
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

export interface AiGatewayInternalError extends Error {}
export interface AiGatewayLogNotFound extends Error {}

export declare abstract class AiGateway {
  patchLog(logId: string, data: AiGatewayPatchLog): Promise<void>;
  getLog(logId: string): Promise<AiGatewayLog>;
}
