export type AiOptions = {
  gateway?: GatewayOptions;
  prefix?: string;
  extraHeaders?: object;
};
export type AiModelType<Name extends keyof AiModels> = AiModels[Name];
export interface AiModels {
  [key: string]: any;
}
export type ModelListType = Record<string, any>;
export type AiModelsSearchParams = {
  author?: string,
  hide_experimental?: boolean
  page?: number
  per_page?: number
  search?: string
  source?: number
  task?: string
}
export type AiModelsSearchObject = {
  id: string,
  source: number,
  name: string,
  description: string,
  task: {
    id: string,
    name: string,
    description: string,
  },
  tags: string[],
  properties: {
    property_id: string,
    value: string,
  }[],
}
export interface InferenceUpstreamError extends Error {}
export interface AiInternalError extends Error {}
export type AiModelListType = Record<string, any>;
export declare abstract class Ai<AiModelList extends AiModelListType = AiModels> {
  aiGatewayLogId: string | null;
  gateway(gatewayId: string): AiGateway;
  /**
    Install @cloudflare/ai-types to get extended TypeScript types for the Workers AI models
    See https://www.npmjs.com/package/@cloudflare/ai-types for more information
  */
  run<Name extends keyof AiModelList>(
    model: Name,
    inputs: AiModelList[Name]["inputs"],
    options?: AiOptions
  ): Promise<AiModelList[Name]["postProcessedOutputs"]>;
  public models(params?: AiModelsSearchParams): Promise<AiModelsSearchObject[]>;
}
