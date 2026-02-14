export type AiImageClassificationInput = {
  image: number[];
};
export type AiImageClassificationOutput = {
  score?: number;
  label?: string;
}[];
export declare abstract class BaseAiImageClassification {
  inputs: AiImageClassificationInput;
  postProcessedOutputs: AiImageClassificationOutput;
}
export type AiImageToTextInput = {
  image: number[];
  prompt?: string;
  max_tokens?: number;
  temperature?: number;
  top_p?: number;
  top_k?: number;
  seed?: number;
  repetition_penalty?: number;
  frequency_penalty?: number;
  presence_penalty?: number;
  raw?: boolean;
  messages?: RoleScopedChatInput[];
};
export type AiImageToTextOutput = {
  description: string;
};
export declare abstract class BaseAiImageToText {
  inputs: AiImageToTextInput;
  postProcessedOutputs: AiImageToTextOutput;
}
export type AiImageTextToTextInput = {
  image: string;
  prompt?: string;
  max_tokens?: number;
  temperature?: number;
  ignore_eos?: boolean;
  top_p?: number;
  top_k?: number;
  seed?: number;
  repetition_penalty?: number;
  frequency_penalty?: number;
  presence_penalty?: number;
  raw?: boolean;
  messages?: RoleScopedChatInput[];
};
export type AiImageTextToTextOutput = {
  description: string;
};
export declare abstract class BaseAiImageTextToText {
  inputs: AiImageTextToTextInput;
  postProcessedOutputs: AiImageTextToTextOutput;
}
export type AiMultimodalEmbeddingsInput = {
  image: string;
  text: string[];
};
export type AiIMultimodalEmbeddingsOutput = {
  data: number[][];
  shape: number[];
};
export declare abstract class BaseAiMultimodalEmbeddings {
  inputs: AiImageTextToTextInput;
  postProcessedOutputs: AiImageTextToTextOutput;
}
export type AiObjectDetectionInput = {
  image: number[];
};
export type AiObjectDetectionOutput = {
  score?: number;
  label?: string;
}[];
export declare abstract class BaseAiObjectDetection {
  inputs: AiObjectDetectionInput;
  postProcessedOutputs: AiObjectDetectionOutput;
}
export type AiSentenceSimilarityInput = {
  source: string;
  sentences: string[];
};
export type AiSentenceSimilarityOutput = number[];
export declare abstract class BaseAiSentenceSimilarity {
  inputs: AiSentenceSimilarityInput;
  postProcessedOutputs: AiSentenceSimilarityOutput;
}
export type AiAutomaticSpeechRecognitionInput = {
  audio: number[];
};
export type AiAutomaticSpeechRecognitionOutput = {
  text?: string;
  words?: {
    word: string;
    start: number;
    end: number;
  }[];
  vtt?: string;
};
export declare abstract class BaseAiAutomaticSpeechRecognition {
  inputs: AiAutomaticSpeechRecognitionInput;
  postProcessedOutputs: AiAutomaticSpeechRecognitionOutput;
}
export type AiSummarizationInput = {
  input_text: string;
  max_length?: number;
};
export type AiSummarizationOutput = {
  summary: string;
};
export declare abstract class BaseAiSummarization {
  inputs: AiSummarizationInput;
  postProcessedOutputs: AiSummarizationOutput;
}
export type AiTextClassificationInput = {
  text: string;
};
export type AiTextClassificationOutput = {
  score?: number;
  label?: string;
}[];
export declare abstract class BaseAiTextClassification {
  inputs: AiTextClassificationInput;
  postProcessedOutputs: AiTextClassificationOutput;
}
export type AiTextEmbeddingsInput = {
  text: string | string[];
};
export type AiTextEmbeddingsOutput = {
  shape: number[];
  data: number[][];
};
export declare abstract class BaseAiTextEmbeddings {
  inputs: AiTextEmbeddingsInput;
  postProcessedOutputs: AiTextEmbeddingsOutput;
}
export type RoleScopedChatInput = {
  role: "user" | "assistant" | "system" | "tool" | (string & NonNullable<unknown>);
  content: string;
  name?: string;
};
export type AiTextGenerationToolLegacyInput = {
  name: string;
  description: string;
  parameters?: {
    type: "object" | (string & NonNullable<unknown>);
    properties: {
      [key: string]: {
        type: string;
        description?: string;
      };
    };
    required: string[];
  };
};
export type AiTextGenerationToolInput = {
  type: "function" | (string & NonNullable<unknown>);
  function: {
    name: string;
    description: string;
    parameters?: {
      type: "object" | (string & NonNullable<unknown>);
      properties: {
        [key: string]: {
          type: string;
          description?: string;
        };
      };
      required: string[];
    };
  };
};
export type AiTextGenerationFunctionsInput = {
  name: string;
  code: string;
};
export type AiTextGenerationResponseFormat = {
  type: string;
  json_schema?: any;
};
export type AiTextGenerationInput = {
  prompt?: string;
  raw?: boolean;
  stream?: boolean;
  max_tokens?: number;
  temperature?: number;
  top_p?: number;
  top_k?: number;
  seed?: number;
  repetition_penalty?: number;
  frequency_penalty?: number;
  presence_penalty?: number;
  messages?: RoleScopedChatInput[];
  response_format?: AiTextGenerationResponseFormat;
  tools?: AiTextGenerationToolInput[] | AiTextGenerationToolLegacyInput[] | (object & NonNullable<unknown>);
  functions?: AiTextGenerationFunctionsInput[];
};
export type AiTextGenerationToolLegacyOutput = {
  name: string;
  arguments: unknown;
};
export type AiTextGenerationToolOutput = {
  id: string;
  type: "function";
  function: {
    name: string;
    arguments: string;
  };
};
export type UsageTags = {
  prompt_tokens: number;
  completion_tokens: number;
  total_tokens: number;
};
export type AiTextGenerationOutput = {
  response?: string;
  tool_calls?: AiTextGenerationToolLegacyOutput[] & AiTextGenerationToolOutput[];
  usage?: UsageTags;
};
export declare abstract class BaseAiTextGeneration {
  inputs: AiTextGenerationInput;
  postProcessedOutputs: AiTextGenerationOutput;
}
export type AiTextToSpeechInput = {
  prompt: string;
  lang?: string;
};
export type AiTextToSpeechOutput =
  | Uint8Array
  | {
      audio: string;
    };
export declare abstract class BaseAiTextToSpeech {
  inputs: AiTextToSpeechInput;
  postProcessedOutputs: AiTextToSpeechOutput;
}
export type AiTextToImageInput = {
  prompt: string;
  negative_prompt?: string;
  height?: number;
  width?: number;
  image?: number[];
  image_b64?: string;
  mask?: number[];
  num_steps?: number;
  strength?: number;
  guidance?: number;
  seed?: number;
};
export type AiTextToImageOutput = ReadableStream<Uint8Array>;
export declare abstract class BaseAiTextToImage {
  inputs: AiTextToImageInput;
  postProcessedOutputs: AiTextToImageOutput;
}
export type AiTranslationInput = {
  text: string;
  target_lang: string;
  source_lang?: string;
};
export type AiTranslationOutput = {
  translated_text?: string;
};
export declare abstract class BaseAiTranslation {
  inputs: AiTranslationInput;
  postProcessedOutputs: AiTranslationOutput;
}
/**
 * Workers AI support for OpenAI's Responses API
 * Reference: https://github.com/openai/openai-node/blob/master/src/resources/responses/responses.ts
 *
 * It's a stripped down version from its source.
 * It currently supports basic function calling, json mode and accepts images as input.
 *
 * It does not include types for WebSearch, CodeInterpreter, FileInputs, MCP, CustomTools.
 * We plan to add those incrementally as model + platform capabilities evolve.
 */
export type ResponsesInput = {
  background?: boolean | null;
  conversation?: string | ResponseConversationParam | null;
  include?: Array<ResponseIncludable> | null;
  input?: string | ResponseInput;
  instructions?: string | null;
  max_output_tokens?: number | null;
  parallel_tool_calls?: boolean | null;
  previous_response_id?: string | null;
  prompt_cache_key?: string;
  reasoning?: Reasoning | null;
  safety_identifier?: string;
  service_tier?: "auto" | "default" | "flex" | "scale" | "priority" | null;
  stream?: boolean | null;
  stream_options?: StreamOptions | null;
  temperature?: number | null;
  text?: ResponseTextConfig;
  tool_choice?: ToolChoiceOptions | ToolChoiceFunction;
  tools?: Array<Tool>;
  top_p?: number | null;
  truncation?: "auto" | "disabled" | null;
};
export type ResponsesOutput = {
  id?: string;
  created_at?: number;
  output_text?: string;
  error?: ResponseError | null;
  incomplete_details?: ResponseIncompleteDetails | null;
  instructions?: string | Array<ResponseInputItem> | null;
  object?: "response";
  output?: Array<ResponseOutputItem>;
  parallel_tool_calls?: boolean;
  temperature?: number | null;
  tool_choice?: ToolChoiceOptions | ToolChoiceFunction;
  tools?: Array<Tool>;
  top_p?: number | null;
  max_output_tokens?: number | null;
  previous_response_id?: string | null;
  prompt?: ResponsePrompt | null;
  reasoning?: Reasoning | null;
  safety_identifier?: string;
  service_tier?: "auto" | "default" | "flex" | "scale" | "priority" | null;
  status?: ResponseStatus;
  text?: ResponseTextConfig;
  truncation?: "auto" | "disabled" | null;
  usage?: ResponseUsage;
};
export type EasyInputMessage = {
  content: string | ResponseInputMessageContentList;
  role: "user" | "assistant" | "system" | "developer";
  type?: "message";
};
export type ResponsesFunctionTool = {
  name: string;
  parameters: {
    [key: string]: unknown;
  } | null;
  strict: boolean | null;
  type: "function";
  description?: string | null;
};
export type ResponseIncompleteDetails = {
  reason?: "max_output_tokens" | "content_filter";
};
export type ResponsePrompt = {
  id: string;
  variables?: {
    [key: string]: string | ResponseInputText | ResponseInputImage;
  } | null;
  version?: string | null;
};
export type Reasoning = {
  effort?: ReasoningEffort | null;
  generate_summary?: "auto" | "concise" | "detailed" | null;
  summary?: "auto" | "concise" | "detailed" | null;
};
export type ResponseContent =
  | ResponseInputText
  | ResponseInputImage
  | ResponseOutputText
  | ResponseOutputRefusal
  | ResponseContentReasoningText;
export type ResponseContentReasoningText = {
  text: string;
  type: "reasoning_text";
};
export type ResponseConversationParam = {
  id: string;
};
export type ResponseCreatedEvent = {
  response: Response;
  sequence_number: number;
  type: "response.created";
};
export type ResponseCustomToolCallOutput = {
  call_id: string;
  output: string | Array<ResponseInputText | ResponseInputImage>;
  type: "custom_tool_call_output";
  id?: string;
};
export type ResponseError = {
  code:
    | "server_error"
    | "rate_limit_exceeded"
    | "invalid_prompt"
    | "vector_store_timeout"
    | "invalid_image"
    | "invalid_image_format"
    | "invalid_base64_image"
    | "invalid_image_url"
    | "image_too_large"
    | "image_too_small"
    | "image_parse_error"
    | "image_content_policy_violation"
    | "invalid_image_mode"
    | "image_file_too_large"
    | "unsupported_image_media_type"
    | "empty_image_file"
    | "failed_to_download_image"
    | "image_file_not_found";
  message: string;
};
export type ResponseErrorEvent = {
  code: string | null;
  message: string;
  param: string | null;
  sequence_number: number;
  type: "error";
};
export type ResponseFailedEvent = {
  response: Response;
  sequence_number: number;
  type: "response.failed";
};
export type ResponseFormatText = {
  type: "text";
};
export type ResponseFormatJSONObject = {
  type: "json_object";
};
export type ResponseFormatTextConfig =
  | ResponseFormatText
  | ResponseFormatTextJSONSchemaConfig
  | ResponseFormatJSONObject;
export type ResponseFormatTextJSONSchemaConfig = {
  name: string;
  schema: {
    [key: string]: unknown;
  };
  type: "json_schema";
  description?: string;
  strict?: boolean | null;
};
export type ResponseFunctionCallArgumentsDeltaEvent = {
  delta: string;
  item_id: string;
  output_index: number;
  sequence_number: number;
  type: "response.function_call_arguments.delta";
};
export type ResponseFunctionCallArgumentsDoneEvent = {
  arguments: string;
  item_id: string;
  name: string;
  output_index: number;
  sequence_number: number;
  type: "response.function_call_arguments.done";
};
export type ResponseFunctionCallOutputItem = ResponseInputTextContent | ResponseInputImageContent;
export type ResponseFunctionCallOutputItemList = Array<ResponseFunctionCallOutputItem>;
export type ResponseFunctionToolCall = {
  arguments: string;
  call_id: string;
  name: string;
  type: "function_call";
  id?: string;
  status?: "in_progress" | "completed" | "incomplete";
};
export interface ResponseFunctionToolCallItem extends ResponseFunctionToolCall {
  id: string;
}
export type ResponseFunctionToolCallOutputItem = {
  id: string;
  call_id: string;
  output: string | Array<ResponseInputText | ResponseInputImage>;
  type: "function_call_output";
  status?: "in_progress" | "completed" | "incomplete";
};
export type ResponseIncludable = "message.input_image.image_url" | "message.output_text.logprobs";
export type ResponseIncompleteEvent = {
  response: Response;
  sequence_number: number;
  type: "response.incomplete";
};
export type ResponseInput = Array<ResponseInputItem>;
export type ResponseInputContent = ResponseInputText | ResponseInputImage;
export type ResponseInputImage = {
  detail: "low" | "high" | "auto";
  type: "input_image";
  /**
   * Base64 encoded image
   */
  image_url?: string | null;
};
export type ResponseInputImageContent = {
  type: "input_image";
  detail?: "low" | "high" | "auto" | null;
  /**
   * Base64 encoded image
   */
  image_url?: string | null;
};
export type ResponseInputItem =
  | EasyInputMessage
  | ResponseInputItemMessage
  | ResponseOutputMessage
  | ResponseFunctionToolCall
  | ResponseInputItemFunctionCallOutput
  | ResponseReasoningItem;
export type ResponseInputItemFunctionCallOutput = {
  call_id: string;
  output: string | ResponseFunctionCallOutputItemList;
  type: "function_call_output";
  id?: string | null;
  status?: "in_progress" | "completed" | "incomplete" | null;
};
export type ResponseInputItemMessage = {
  content: ResponseInputMessageContentList;
  role: "user" | "system" | "developer";
  status?: "in_progress" | "completed" | "incomplete";
  type?: "message";
};
export type ResponseInputMessageContentList = Array<ResponseInputContent>;
export type ResponseInputMessageItem = {
  id: string;
  content: ResponseInputMessageContentList;
  role: "user" | "system" | "developer";
  status?: "in_progress" | "completed" | "incomplete";
  type?: "message";
};
export type ResponseInputText = {
  text: string;
  type: "input_text";
};
export type ResponseInputTextContent = {
  text: string;
  type: "input_text";
};
export type ResponseItem =
  | ResponseInputMessageItem
  | ResponseOutputMessage
  | ResponseFunctionToolCallItem
  | ResponseFunctionToolCallOutputItem;
export type ResponseOutputItem = ResponseOutputMessage | ResponseFunctionToolCall | ResponseReasoningItem;
export type ResponseOutputItemAddedEvent = {
  item: ResponseOutputItem;
  output_index: number;
  sequence_number: number;
  type: "response.output_item.added";
};
export type ResponseOutputItemDoneEvent = {
  item: ResponseOutputItem;
  output_index: number;
  sequence_number: number;
  type: "response.output_item.done";
};
export type ResponseOutputMessage = {
  id: string;
  content: Array<ResponseOutputText | ResponseOutputRefusal>;
  role: "assistant";
  status: "in_progress" | "completed" | "incomplete";
  type: "message";
};
export type ResponseOutputRefusal = {
  refusal: string;
  type: "refusal";
};
export type ResponseOutputText = {
  text: string;
  type: "output_text";
  logprobs?: Array<Logprob>;
};
export type ResponseReasoningItem = {
  id: string;
  summary: Array<ResponseReasoningSummaryItem>;
  type: "reasoning";
  content?: Array<ResponseReasoningContentItem>;
  encrypted_content?: string | null;
  status?: "in_progress" | "completed" | "incomplete";
};
export type ResponseReasoningSummaryItem = {
  text: string;
  type: "summary_text";
};
export type ResponseReasoningContentItem = {
  text: string;
  type: "reasoning_text";
};
export type ResponseReasoningTextDeltaEvent = {
  content_index: number;
  delta: string;
  item_id: string;
  output_index: number;
  sequence_number: number;
  type: "response.reasoning_text.delta";
};
export type ResponseReasoningTextDoneEvent = {
  content_index: number;
  item_id: string;
  output_index: number;
  sequence_number: number;
  text: string;
  type: "response.reasoning_text.done";
};
export type ResponseRefusalDeltaEvent = {
  content_index: number;
  delta: string;
  item_id: string;
  output_index: number;
  sequence_number: number;
  type: "response.refusal.delta";
};
export type ResponseRefusalDoneEvent = {
  content_index: number;
  item_id: string;
  output_index: number;
  refusal: string;
  sequence_number: number;
  type: "response.refusal.done";
};
export type ResponseStatus = "completed" | "failed" | "in_progress" | "cancelled" | "queued" | "incomplete";
export type ResponseStreamEvent =
  | ResponseCompletedEvent
  | ResponseCreatedEvent
  | ResponseErrorEvent
  | ResponseFunctionCallArgumentsDeltaEvent
  | ResponseFunctionCallArgumentsDoneEvent
  | ResponseFailedEvent
  | ResponseIncompleteEvent
  | ResponseOutputItemAddedEvent
  | ResponseOutputItemDoneEvent
  | ResponseReasoningTextDeltaEvent
  | ResponseReasoningTextDoneEvent
  | ResponseRefusalDeltaEvent
  | ResponseRefusalDoneEvent
  | ResponseTextDeltaEvent
  | ResponseTextDoneEvent;
export type ResponseCompletedEvent = {
  response: Response;
  sequence_number: number;
  type: "response.completed";
};
export type ResponseTextConfig = {
  format?: ResponseFormatTextConfig;
  verbosity?: "low" | "medium" | "high" | null;
};
export type ResponseTextDeltaEvent = {
  content_index: number;
  delta: string;
  item_id: string;
  logprobs: Array<Logprob>;
  output_index: number;
  sequence_number: number;
  type: "response.output_text.delta";
};
export type ResponseTextDoneEvent = {
  content_index: number;
  item_id: string;
  logprobs: Array<Logprob>;
  output_index: number;
  sequence_number: number;
  text: string;
  type: "response.output_text.done";
};
export type Logprob = {
  token: string;
  logprob: number;
  top_logprobs?: Array<TopLogprob>;
};
export type TopLogprob = {
  token?: string;
  logprob?: number;
};
export type ResponseUsage = {
  input_tokens: number;
  output_tokens: number;
  total_tokens: number;
};
export type Tool = ResponsesFunctionTool;
export type ToolChoiceFunction = {
  name: string;
  type: "function";
};
export type ToolChoiceOptions = "none";
export type ReasoningEffort = "minimal" | "low" | "medium" | "high" | null;
export type StreamOptions = {
  include_obfuscation?: boolean;
};
/** Marks keys from T that aren't in U as optional never */
export type Without<T, U> = {
  [P in Exclude<keyof T, keyof U>]?: never;
};
/** Either T or U, but not both (mutually exclusive) */
export type XOR<T, U> = (T & Without<U, T>) | (U & Without<T, U>);
export type Ai_Cf_Baai_Bge_Base_En_V1_5_Input =
  | {
      text: string | string[];
      /**
       * The pooling method used in the embedding process. `cls` pooling will generate more accurate embeddings on larger inputs - however, embeddings created with cls pooling are not compatible with embeddings generated with mean pooling. The default pooling method is `mean` in order for this to not be a breaking change, but we highly suggest using the new `cls` pooling for better accuracy.
       */
      pooling?: "mean" | "cls";
    }
  | {
      /**
       * Batch of the embeddings requests to run using async-queue
       */
      requests: {
        text: string | string[];
        /**
         * The pooling method used in the embedding process. `cls` pooling will generate more accurate embeddings on larger inputs - however, embeddings created with cls pooling are not compatible with embeddings generated with mean pooling. The default pooling method is `mean` in order for this to not be a breaking change, but we highly suggest using the new `cls` pooling for better accuracy.
         */
        pooling?: "mean" | "cls";
      }[];
    };
export type Ai_Cf_Baai_Bge_Base_En_V1_5_Output =
  | {
      shape?: number[];
      /**
       * Embeddings of the requested text values
       */
      data?: number[][];
      /**
       * The pooling method used in the embedding process.
       */
      pooling?: "mean" | "cls";
    }
  | Ai_Cf_Baai_Bge_Base_En_V1_5_AsyncResponse;
export interface Ai_Cf_Baai_Bge_Base_En_V1_5_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Baai_Bge_Base_En_V1_5 {
  inputs: Ai_Cf_Baai_Bge_Base_En_V1_5_Input;
  postProcessedOutputs: Ai_Cf_Baai_Bge_Base_En_V1_5_Output;
}
export type Ai_Cf_Openai_Whisper_Input =
  | string
  | {
      /**
       * An array of integers that represent the audio data constrained to 8-bit unsigned integer values
       */
      audio: number[];
    };
export interface Ai_Cf_Openai_Whisper_Output {
  /**
   * The transcription
   */
  text: string;
  word_count?: number;
  words?: {
    word?: string;
    /**
     * The second this word begins in the recording
     */
    start?: number;
    /**
     * The ending second when the word completes
     */
    end?: number;
  }[];
  vtt?: string;
}
export declare abstract class Base_Ai_Cf_Openai_Whisper {
  inputs: Ai_Cf_Openai_Whisper_Input;
  postProcessedOutputs: Ai_Cf_Openai_Whisper_Output;
}
export type Ai_Cf_Meta_M2M100_1_2B_Input =
  | {
      /**
       * The text to be translated
       */
      text: string;
      /**
       * The language code of the source text (e.g., 'en' for English). Defaults to 'en' if not specified
       */
      source_lang?: string;
      /**
       * The language code to translate the text into (e.g., 'es' for Spanish)
       */
      target_lang: string;
    }
  | {
      /**
       * Batch of the embeddings requests to run using async-queue
       */
      requests: {
        /**
         * The text to be translated
         */
        text: string;
        /**
         * The language code of the source text (e.g., 'en' for English). Defaults to 'en' if not specified
         */
        source_lang?: string;
        /**
         * The language code to translate the text into (e.g., 'es' for Spanish)
         */
        target_lang: string;
      }[];
    };
export type Ai_Cf_Meta_M2M100_1_2B_Output =
  | {
      /**
       * The translated text in the target language
       */
      translated_text?: string;
    }
  | Ai_Cf_Meta_M2M100_1_2B_AsyncResponse;
export interface Ai_Cf_Meta_M2M100_1_2B_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Meta_M2M100_1_2B {
  inputs: Ai_Cf_Meta_M2M100_1_2B_Input;
  postProcessedOutputs: Ai_Cf_Meta_M2M100_1_2B_Output;
}
export type Ai_Cf_Baai_Bge_Small_En_V1_5_Input =
  | {
      text: string | string[];
      /**
       * The pooling method used in the embedding process. `cls` pooling will generate more accurate embeddings on larger inputs - however, embeddings created with cls pooling are not compatible with embeddings generated with mean pooling. The default pooling method is `mean` in order for this to not be a breaking change, but we highly suggest using the new `cls` pooling for better accuracy.
       */
      pooling?: "mean" | "cls";
    }
  | {
      /**
       * Batch of the embeddings requests to run using async-queue
       */
      requests: {
        text: string | string[];
        /**
         * The pooling method used in the embedding process. `cls` pooling will generate more accurate embeddings on larger inputs - however, embeddings created with cls pooling are not compatible with embeddings generated with mean pooling. The default pooling method is `mean` in order for this to not be a breaking change, but we highly suggest using the new `cls` pooling for better accuracy.
         */
        pooling?: "mean" | "cls";
      }[];
    };
export type Ai_Cf_Baai_Bge_Small_En_V1_5_Output =
  | {
      shape?: number[];
      /**
       * Embeddings of the requested text values
       */
      data?: number[][];
      /**
       * The pooling method used in the embedding process.
       */
      pooling?: "mean" | "cls";
    }
  | Ai_Cf_Baai_Bge_Small_En_V1_5_AsyncResponse;
export interface Ai_Cf_Baai_Bge_Small_En_V1_5_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Baai_Bge_Small_En_V1_5 {
  inputs: Ai_Cf_Baai_Bge_Small_En_V1_5_Input;
  postProcessedOutputs: Ai_Cf_Baai_Bge_Small_En_V1_5_Output;
}
export type Ai_Cf_Baai_Bge_Large_En_V1_5_Input =
  | {
      text: string | string[];
      /**
       * The pooling method used in the embedding process. `cls` pooling will generate more accurate embeddings on larger inputs - however, embeddings created with cls pooling are not compatible with embeddings generated with mean pooling. The default pooling method is `mean` in order for this to not be a breaking change, but we highly suggest using the new `cls` pooling for better accuracy.
       */
      pooling?: "mean" | "cls";
    }
  | {
      /**
       * Batch of the embeddings requests to run using async-queue
       */
      requests: {
        text: string | string[];
        /**
         * The pooling method used in the embedding process. `cls` pooling will generate more accurate embeddings on larger inputs - however, embeddings created with cls pooling are not compatible with embeddings generated with mean pooling. The default pooling method is `mean` in order for this to not be a breaking change, but we highly suggest using the new `cls` pooling for better accuracy.
         */
        pooling?: "mean" | "cls";
      }[];
    };
export type Ai_Cf_Baai_Bge_Large_En_V1_5_Output =
  | {
      shape?: number[];
      /**
       * Embeddings of the requested text values
       */
      data?: number[][];
      /**
       * The pooling method used in the embedding process.
       */
      pooling?: "mean" | "cls";
    }
  | Ai_Cf_Baai_Bge_Large_En_V1_5_AsyncResponse;
export interface Ai_Cf_Baai_Bge_Large_En_V1_5_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Baai_Bge_Large_En_V1_5 {
  inputs: Ai_Cf_Baai_Bge_Large_En_V1_5_Input;
  postProcessedOutputs: Ai_Cf_Baai_Bge_Large_En_V1_5_Output;
}
export type Ai_Cf_Unum_Uform_Gen2_Qwen_500M_Input =
  | string
  | {
      /**
       * The input text prompt for the model to generate a response.
       */
      prompt?: string;
      /**
       * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
       */
      raw?: boolean;
      /**
       * Controls the creativity of the AI's responses by adjusting how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
       */
      top_p?: number;
      /**
       * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
       */
      top_k?: number;
      /**
       * Random seed for reproducibility of the generation.
       */
      seed?: number;
      /**
       * Penalty for repeated tokens; higher values discourage repetition.
       */
      repetition_penalty?: number;
      /**
       * Decreases the likelihood of the model repeating the same lines verbatim.
       */
      frequency_penalty?: number;
      /**
       * Increases the likelihood of the model introducing new topics.
       */
      presence_penalty?: number;
      image: number[] | (string & NonNullable<unknown>);
      /**
       * The maximum number of tokens to generate in the response.
       */
      max_tokens?: number;
    };
export interface Ai_Cf_Unum_Uform_Gen2_Qwen_500M_Output {
  description?: string;
}
export declare abstract class Base_Ai_Cf_Unum_Uform_Gen2_Qwen_500M {
  inputs: Ai_Cf_Unum_Uform_Gen2_Qwen_500M_Input;
  postProcessedOutputs: Ai_Cf_Unum_Uform_Gen2_Qwen_500M_Output;
}
export type Ai_Cf_Openai_Whisper_Tiny_En_Input =
  | string
  | {
      /**
       * An array of integers that represent the audio data constrained to 8-bit unsigned integer values
       */
      audio: number[];
    };
export interface Ai_Cf_Openai_Whisper_Tiny_En_Output {
  /**
   * The transcription
   */
  text: string;
  word_count?: number;
  words?: {
    word?: string;
    /**
     * The second this word begins in the recording
     */
    start?: number;
    /**
     * The ending second when the word completes
     */
    end?: number;
  }[];
  vtt?: string;
}
export declare abstract class Base_Ai_Cf_Openai_Whisper_Tiny_En {
  inputs: Ai_Cf_Openai_Whisper_Tiny_En_Input;
  postProcessedOutputs: Ai_Cf_Openai_Whisper_Tiny_En_Output;
}
export interface Ai_Cf_Openai_Whisper_Large_V3_Turbo_Input {
  /**
   * Base64 encoded value of the audio data.
   */
  audio: string;
  /**
   * Supported tasks are 'translate' or 'transcribe'.
   */
  task?: string;
  /**
   * The language of the audio being transcribed or translated.
   */
  language?: string;
  /**
   * Preprocess the audio with a voice activity detection model.
   */
  vad_filter?: boolean;
  /**
   * A text prompt to help provide context to the model on the contents of the audio.
   */
  initial_prompt?: string;
  /**
   * The prefix it appended the the beginning of the output of the transcription and can guide the transcription result.
   */
  prefix?: string;
}
export interface Ai_Cf_Openai_Whisper_Large_V3_Turbo_Output {
  transcription_info?: {
    /**
     * The language of the audio being transcribed or translated.
     */
    language?: string;
    /**
     * The confidence level or probability of the detected language being accurate, represented as a decimal between 0 and 1.
     */
    language_probability?: number;
    /**
     * The total duration of the original audio file, in seconds.
     */
    duration?: number;
    /**
     * The duration of the audio after applying Voice Activity Detection (VAD) to remove silent or irrelevant sections, in seconds.
     */
    duration_after_vad?: number;
  };
  /**
   * The complete transcription of the audio.
   */
  text: string;
  /**
   * The total number of words in the transcription.
   */
  word_count?: number;
  segments?: {
    /**
     * The starting time of the segment within the audio, in seconds.
     */
    start?: number;
    /**
     * The ending time of the segment within the audio, in seconds.
     */
    end?: number;
    /**
     * The transcription of the segment.
     */
    text?: string;
    /**
     * The temperature used in the decoding process, controlling randomness in predictions. Lower values result in more deterministic outputs.
     */
    temperature?: number;
    /**
     * The average log probability of the predictions for the words in this segment, indicating overall confidence.
     */
    avg_logprob?: number;
    /**
     * The compression ratio of the input to the output, measuring how much the text was compressed during the transcription process.
     */
    compression_ratio?: number;
    /**
     * The probability that the segment contains no speech, represented as a decimal between 0 and 1.
     */
    no_speech_prob?: number;
    words?: {
      /**
       * The individual word transcribed from the audio.
       */
      word?: string;
      /**
       * The starting time of the word within the audio, in seconds.
       */
      start?: number;
      /**
       * The ending time of the word within the audio, in seconds.
       */
      end?: number;
    }[];
  }[];
  /**
   * The transcription in WebVTT format, which includes timing and text information for use in subtitles.
   */
  vtt?: string;
}
export declare abstract class Base_Ai_Cf_Openai_Whisper_Large_V3_Turbo {
  inputs: Ai_Cf_Openai_Whisper_Large_V3_Turbo_Input;
  postProcessedOutputs: Ai_Cf_Openai_Whisper_Large_V3_Turbo_Output;
}
export type Ai_Cf_Baai_Bge_M3_Input =
  | Ai_Cf_Baai_Bge_M3_Input_QueryAnd_Contexts
  | Ai_Cf_Baai_Bge_M3_Input_Embedding
  | {
      /**
       * Batch of the embeddings requests to run using async-queue
       */
      requests: (Ai_Cf_Baai_Bge_M3_Input_QueryAnd_Contexts_1 | Ai_Cf_Baai_Bge_M3_Input_Embedding_1)[];
    };
export interface Ai_Cf_Baai_Bge_M3_Input_QueryAnd_Contexts {
  /**
   * A query you wish to perform against the provided contexts. If no query is provided the model with respond with embeddings for contexts
   */
  query?: string;
  /**
   * List of provided contexts. Note that the index in this array is important, as the response will refer to it.
   */
  contexts: {
    /**
     * One of the provided context content
     */
    text?: string;
  }[];
  /**
   * When provided with too long context should the model error out or truncate the context to fit?
   */
  truncate_inputs?: boolean;
}
export interface Ai_Cf_Baai_Bge_M3_Input_Embedding {
  text: string | string[];
  /**
   * When provided with too long context should the model error out or truncate the context to fit?
   */
  truncate_inputs?: boolean;
}
export interface Ai_Cf_Baai_Bge_M3_Input_QueryAnd_Contexts_1 {
  /**
   * A query you wish to perform against the provided contexts. If no query is provided the model with respond with embeddings for contexts
   */
  query?: string;
  /**
   * List of provided contexts. Note that the index in this array is important, as the response will refer to it.
   */
  contexts: {
    /**
     * One of the provided context content
     */
    text?: string;
  }[];
  /**
   * When provided with too long context should the model error out or truncate the context to fit?
   */
  truncate_inputs?: boolean;
}
export interface Ai_Cf_Baai_Bge_M3_Input_Embedding_1 {
  text: string | string[];
  /**
   * When provided with too long context should the model error out or truncate the context to fit?
   */
  truncate_inputs?: boolean;
}
export type Ai_Cf_Baai_Bge_M3_Output =
  | Ai_Cf_Baai_Bge_M3_Ouput_Query
  | Ai_Cf_Baai_Bge_M3_Output_EmbeddingFor_Contexts
  | Ai_Cf_Baai_Bge_M3_Ouput_Embedding
  | Ai_Cf_Baai_Bge_M3_AsyncResponse;
export interface Ai_Cf_Baai_Bge_M3_Ouput_Query {
  response?: {
    /**
     * Index of the context in the request
     */
    id?: number;
    /**
     * Score of the context under the index.
     */
    score?: number;
  }[];
}
export interface Ai_Cf_Baai_Bge_M3_Output_EmbeddingFor_Contexts {
  response?: number[][];
  shape?: number[];
  /**
   * The pooling method used in the embedding process.
   */
  pooling?: "mean" | "cls";
}
export interface Ai_Cf_Baai_Bge_M3_Ouput_Embedding {
  shape?: number[];
  /**
   * Embeddings of the requested text values
   */
  data?: number[][];
  /**
   * The pooling method used in the embedding process.
   */
  pooling?: "mean" | "cls";
}
export interface Ai_Cf_Baai_Bge_M3_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Baai_Bge_M3 {
  inputs: Ai_Cf_Baai_Bge_M3_Input;
  postProcessedOutputs: Ai_Cf_Baai_Bge_M3_Output;
}
export interface Ai_Cf_Black_Forest_Labs_Flux_1_Schnell_Input {
  /**
   * A text description of the image you want to generate.
   */
  prompt: string;
  /**
   * The number of diffusion steps; higher values can improve quality but take longer.
   */
  steps?: number;
}
export interface Ai_Cf_Black_Forest_Labs_Flux_1_Schnell_Output {
  /**
   * The generated image in Base64 format.
   */
  image?: string;
}
export declare abstract class Base_Ai_Cf_Black_Forest_Labs_Flux_1_Schnell {
  inputs: Ai_Cf_Black_Forest_Labs_Flux_1_Schnell_Input;
  postProcessedOutputs: Ai_Cf_Black_Forest_Labs_Flux_1_Schnell_Output;
}
export type Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Input =
  | Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Prompt
  | Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Messages;
export interface Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  image?: number[] | (string & NonNullable<unknown>);
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
  /**
   * Name of the LoRA (Low-Rank Adaptation) model to fine-tune the base model.
   */
  lora?: string;
}
export interface Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role?: string;
    /**
     * The tool call id. Must be supplied for tool calls for Mistral-3. If you don't know what to put here you can fall back to 000000001
     */
    tool_call_id?: string;
    content?:
      | string
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        }[]
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        };
  }[];
  image?: number[] | (string & NonNullable<unknown>);
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  /**
   * If true, the response will be streamed back incrementally.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Controls the creativity of the AI's responses by adjusting how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export type Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Output = {
  /**
   * The generated text response from the model
   */
  response?: string;
  /**
   * An array of tool calls requests made during the response generation
   */
  tool_calls?: {
    /**
     * The arguments passed to be passed to the tool call request
     */
    arguments?: object;
    /**
     * The name of the tool to be called
     */
    name?: string;
  }[];
};
export declare abstract class Base_Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct {
  inputs: Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Input;
  postProcessedOutputs: Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Output;
}
export type Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Input =
  | Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Prompt
  | Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Messages
  | Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Async_Batch;
export interface Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * Name of the LoRA (Low-Rank Adaptation) model to fine-tune the base model.
   */
  lora?: string;
  response_format?: Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_JSON_Mode;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_JSON_Mode {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role: string;
    /**
     * The content of the message as a string.
     */
    content: string;
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_JSON_Mode_1;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_JSON_Mode_1 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Async_Batch {
  requests?: {
    /**
     * User-supplied reference. This field will be present in the response as well it can be used to reference the request and response. It's NOT validated to be unique.
     */
    external_reference?: string;
    /**
     * Prompt for the text generation model
     */
    prompt?: string;
    /**
     * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
     */
    stream?: boolean;
    /**
     * The maximum number of tokens to generate in the response.
     */
    max_tokens?: number;
    /**
     * Controls the randomness of the output; higher values produce more random results.
     */
    temperature?: number;
    /**
     * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
     */
    top_p?: number;
    /**
     * Random seed for reproducibility of the generation.
     */
    seed?: number;
    /**
     * Penalty for repeated tokens; higher values discourage repetition.
     */
    repetition_penalty?: number;
    /**
     * Decreases the likelihood of the model repeating the same lines verbatim.
     */
    frequency_penalty?: number;
    /**
     * Increases the likelihood of the model introducing new topics.
     */
    presence_penalty?: number;
    response_format?: Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_JSON_Mode_2;
  }[];
}
export interface Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_JSON_Mode_2 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export type Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Output =
  | {
      /**
       * The generated text response from the model
       */
      response: string;
      /**
       * Usage statistics for the inference request
       */
      usage?: {
        /**
         * Total number of tokens in input
         */
        prompt_tokens?: number;
        /**
         * Total number of tokens in output
         */
        completion_tokens?: number;
        /**
         * Total number of input and output tokens
         */
        total_tokens?: number;
      };
      /**
       * An array of tool calls requests made during the response generation
       */
      tool_calls?: {
        /**
         * The arguments passed to be passed to the tool call request
         */
        arguments?: object;
        /**
         * The name of the tool to be called
         */
        name?: string;
      }[];
    }
  | string
  | Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_AsyncResponse;
export interface Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast {
  inputs: Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Input;
  postProcessedOutputs: Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast_Output;
}
export interface Ai_Cf_Meta_Llama_Guard_3_8B_Input {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender must alternate between 'user' and 'assistant'.
     */
    role: "user" | "assistant";
    /**
     * The content of the message as a string.
     */
    content: string;
  }[];
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Dictate the output format of the generated response.
   */
  response_format?: {
    /**
     * Set to json_object to process and output generated text as JSON.
     */
    type?: string;
  };
}
export interface Ai_Cf_Meta_Llama_Guard_3_8B_Output {
  response?:
    | string
    | {
        /**
         * Whether the conversation is safe or not.
         */
        safe?: boolean;
        /**
         * A list of what hazard categories predicted for the conversation, if the conversation is deemed unsafe.
         */
        categories?: string[];
      };
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
}
export declare abstract class Base_Ai_Cf_Meta_Llama_Guard_3_8B {
  inputs: Ai_Cf_Meta_Llama_Guard_3_8B_Input;
  postProcessedOutputs: Ai_Cf_Meta_Llama_Guard_3_8B_Output;
}
export interface Ai_Cf_Baai_Bge_Reranker_Base_Input {
  /**
   * A query you wish to perform against the provided contexts.
   */
  /**
   * Number of returned results starting with the best score.
   */
  top_k?: number;
  /**
   * List of provided contexts. Note that the index in this array is important, as the response will refer to it.
   */
  contexts: {
    /**
     * One of the provided context content
     */
    text?: string;
  }[];
}
export interface Ai_Cf_Baai_Bge_Reranker_Base_Output {
  response?: {
    /**
     * Index of the context in the request
     */
    id?: number;
    /**
     * Score of the context under the index.
     */
    score?: number;
  }[];
}
export declare abstract class Base_Ai_Cf_Baai_Bge_Reranker_Base {
  inputs: Ai_Cf_Baai_Bge_Reranker_Base_Input;
  postProcessedOutputs: Ai_Cf_Baai_Bge_Reranker_Base_Output;
}
export type Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Input =
  | Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Prompt
  | Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Messages;
export interface Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * Name of the LoRA (Low-Rank Adaptation) model to fine-tune the base model.
   */
  lora?: string;
  response_format?: Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_JSON_Mode;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_JSON_Mode {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role: string;
    /**
     * The content of the message as a string.
     */
    content: string;
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_JSON_Mode_1;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_JSON_Mode_1 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export type Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Output = {
  /**
   * The generated text response from the model
   */
  response: string;
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
  /**
   * An array of tool calls requests made during the response generation
   */
  tool_calls?: {
    /**
     * The arguments passed to be passed to the tool call request
     */
    arguments?: object;
    /**
     * The name of the tool to be called
     */
    name?: string;
  }[];
};
export declare abstract class Base_Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct {
  inputs: Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Input;
  postProcessedOutputs: Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct_Output;
}
export type Ai_Cf_Qwen_Qwq_32B_Input = Ai_Cf_Qwen_Qwq_32B_Prompt | Ai_Cf_Qwen_Qwq_32B_Messages;
export interface Ai_Cf_Qwen_Qwq_32B_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Qwen_Qwq_32B_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role?: string;
    /**
     * The tool call id. Must be supplied for tool calls for Mistral-3. If you don't know what to put here you can fall back to 000000001
     */
    tool_call_id?: string;
    content?:
      | string
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        }[]
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        };
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export type Ai_Cf_Qwen_Qwq_32B_Output = {
  /**
   * The generated text response from the model
   */
  response: string;
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
  /**
   * An array of tool calls requests made during the response generation
   */
  tool_calls?: {
    /**
     * The arguments passed to be passed to the tool call request
     */
    arguments?: object;
    /**
     * The name of the tool to be called
     */
    name?: string;
  }[];
};
export declare abstract class Base_Ai_Cf_Qwen_Qwq_32B {
  inputs: Ai_Cf_Qwen_Qwq_32B_Input;
  postProcessedOutputs: Ai_Cf_Qwen_Qwq_32B_Output;
}
export type Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Input =
  | Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Prompt
  | Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Messages;
export interface Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role?: string;
    /**
     * The tool call id. Must be supplied for tool calls for Mistral-3. If you don't know what to put here you can fall back to 000000001
     */
    tool_call_id?: string;
    content?:
      | string
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        }[]
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        };
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export type Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Output = {
  /**
   * The generated text response from the model
   */
  response: string;
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
  /**
   * An array of tool calls requests made during the response generation
   */
  tool_calls?: {
    /**
     * The arguments passed to be passed to the tool call request
     */
    arguments?: object;
    /**
     * The name of the tool to be called
     */
    name?: string;
  }[];
};
export declare abstract class Base_Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct {
  inputs: Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Input;
  postProcessedOutputs: Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct_Output;
}
export type Ai_Cf_Google_Gemma_3_12B_It_Input =
  | Ai_Cf_Google_Gemma_3_12B_It_Prompt
  | Ai_Cf_Google_Gemma_3_12B_It_Messages;
export interface Ai_Cf_Google_Gemma_3_12B_It_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Google_Gemma_3_12B_It_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role?: string;
    content?:
      | string
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        }[];
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export type Ai_Cf_Google_Gemma_3_12B_It_Output = {
  /**
   * The generated text response from the model
   */
  response: string;
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
  /**
   * An array of tool calls requests made during the response generation
   */
  tool_calls?: {
    /**
     * The arguments passed to be passed to the tool call request
     */
    arguments?: object;
    /**
     * The name of the tool to be called
     */
    name?: string;
  }[];
};
export declare abstract class Base_Ai_Cf_Google_Gemma_3_12B_It {
  inputs: Ai_Cf_Google_Gemma_3_12B_It_Input;
  postProcessedOutputs: Ai_Cf_Google_Gemma_3_12B_It_Output;
}
export type Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Input =
  | Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Prompt
  | Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Messages
  | Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Async_Batch;
export interface Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  response_format?: Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_JSON_Mode;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_JSON_Mode {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role?: string;
    /**
     * The tool call id. If you don't know what to put here you can fall back to 000000001
     */
    tool_call_id?: string;
    content?:
      | string
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        }[]
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        };
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_JSON_Mode;
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Async_Batch {
  requests: (
    | Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Prompt_Inner
    | Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Messages_Inner
  )[];
}
export interface Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Prompt_Inner {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  response_format?: Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_JSON_Mode;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Messages_Inner {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role?: string;
    /**
     * The tool call id. If you don't know what to put here you can fall back to 000000001
     */
    tool_call_id?: string;
    content?:
      | string
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        }[]
      | {
          /**
           * Type of the content provided
           */
          type?: string;
          text?: string;
          image_url?: {
            /**
             * image uri with data (e.g. data:image/jpeg;base64,/9j/...). HTTP URL will not be accepted
             */
            url?: string;
          };
        };
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_JSON_Mode;
  /**
   * JSON schema that should be fulfilled for the response.
   */
  guided_json?: object;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export type Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Output = {
  /**
   * The generated text response from the model
   */
  response: string;
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
  /**
   * An array of tool calls requests made during the response generation
   */
  tool_calls?: {
    /**
     * The tool call id.
     */
    id?: string;
    /**
     * Specifies the type of tool (e.g., 'function').
     */
    type?: string;
    /**
     * Details of the function tool.
     */
    function?: {
      /**
       * The name of the tool to be called
       */
      name?: string;
      /**
       * The arguments passed to be passed to the tool call request
       */
      arguments?: object;
    };
  }[];
};
export declare abstract class Base_Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct {
  inputs: Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Input;
  postProcessedOutputs: Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct_Output;
}
export type Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Input =
  | Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Prompt
  | Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Messages
  | Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Async_Batch;
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * Name of the LoRA (Low-Rank Adaptation) model to fine-tune the base model.
   */
  lora?: string;
  response_format?: Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role: string;
    /**
     * The content of the message as a string.
     */
    content: string;
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode_1;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode_1 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Async_Batch {
  requests: (Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Prompt_1 | Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Messages_1)[];
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Prompt_1 {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * Name of the LoRA (Low-Rank Adaptation) model to fine-tune the base model.
   */
  lora?: string;
  response_format?: Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode_2;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode_2 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Messages_1 {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role: string;
    /**
     * The content of the message as a string.
     */
    content: string;
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode_3;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_JSON_Mode_3 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export type Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Output =
  | Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Chat_Completion_Response
  | Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Text_Completion_Response
  | string
  | Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_AsyncResponse;
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Chat_Completion_Response {
  /**
   * Unique identifier for the completion
   */
  id?: string;
  /**
   * Object type identifier
   */
  object?: "chat.completion";
  /**
   * Unix timestamp of when the completion was created
   */
  created?: number;
  /**
   * Model used for the completion
   */
  model?: string;
  /**
   * List of completion choices
   */
  choices?: {
    /**
     * Index of the choice in the list
     */
    index?: number;
    /**
     * The message generated by the model
     */
    message?: {
      /**
       * Role of the message author
       */
      role: string;
      /**
       * The content of the message
       */
      content: string;
      /**
       * Internal reasoning content (if available)
       */
      reasoning_content?: string;
      /**
       * Tool calls made by the assistant
       */
      tool_calls?: {
        /**
         * Unique identifier for the tool call
         */
        id: string;
        /**
         * Type of tool call
         */
        type: "function";
        function: {
          /**
           * Name of the function to call
           */
          name: string;
          /**
           * JSON string of arguments for the function
           */
          arguments: string;
        };
      }[];
    };
    /**
     * Reason why the model stopped generating
     */
    finish_reason?: string;
    /**
     * Stop reason (may be null)
     */
    stop_reason?: string | null;
    /**
     * Log probabilities (if requested)
     */
    logprobs?: {} | null;
  }[];
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
  /**
   * Log probabilities for the prompt (if requested)
   */
  prompt_logprobs?: {} | null;
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Text_Completion_Response {
  /**
   * Unique identifier for the completion
   */
  id?: string;
  /**
   * Object type identifier
   */
  object?: "text_completion";
  /**
   * Unix timestamp of when the completion was created
   */
  created?: number;
  /**
   * Model used for the completion
   */
  model?: string;
  /**
   * List of completion choices
   */
  choices?: {
    /**
     * Index of the choice in the list
     */
    index: number;
    /**
     * The generated text completion
     */
    text: string;
    /**
     * Reason why the model stopped generating
     */
    finish_reason: string;
    /**
     * Stop reason (may be null)
     */
    stop_reason?: string | null;
    /**
     * Log probabilities (if requested)
     */
    logprobs?: {} | null;
    /**
     * Log probabilities for the prompt (if requested)
     */
    prompt_logprobs?: {} | null;
  }[];
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
}
export interface Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8 {
  inputs: Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Input;
  postProcessedOutputs: Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8_Output;
}
export interface Ai_Cf_Deepgram_Nova_3_Input {
  audio: {
    body: object;
    contentType: string;
  };
  /**
   * Sets how the model will interpret strings submitted to the custom_topic param. When strict, the model will only return topics submitted using the custom_topic param. When extended, the model will return its own detected topics in addition to those submitted using the custom_topic param.
   */
  custom_topic_mode?: "extended" | "strict";
  /**
   * Custom topics you want the model to detect within your input audio or text if present Submit up to 100
   */
  custom_topic?: string;
  /**
   * Sets how the model will interpret intents submitted to the custom_intent param. When strict, the model will only return intents submitted using the custom_intent param. When extended, the model will return its own detected intents in addition those submitted using the custom_intents param
   */
  custom_intent_mode?: "extended" | "strict";
  /**
   * Custom intents you want the model to detect within your input audio if present
   */
  custom_intent?: string;
  /**
   * Identifies and extracts key entities from content in submitted audio
   */
  detect_entities?: boolean;
  /**
   * Identifies the dominant language spoken in submitted audio
   */
  detect_language?: boolean;
  /**
   * Recognize speaker changes. Each word in the transcript will be assigned a speaker number starting at 0
   */
  diarize?: boolean;
  /**
   * Identify and extract key entities from content in submitted audio
   */
  dictation?: boolean;
  /**
   * Specify the expected encoding of your submitted audio
   */
  encoding?: "linear16" | "flac" | "mulaw" | "amr-nb" | "amr-wb" | "opus" | "speex" | "g729";
  /**
   * Arbitrary key-value pairs that are attached to the API response for usage in downstream processing
   */
  extra?: string;
  /**
   * Filler Words can help transcribe interruptions in your audio, like 'uh' and 'um'
   */
  filler_words?: boolean;
  /**
   * Key term prompting can boost or suppress specialized terminology and brands.
   */
  keyterm?: string;
  /**
   * Keywords can boost or suppress specialized terminology and brands.
   */
  keywords?: string;
  /**
   * The BCP-47 language tag that hints at the primary spoken language. Depending on the Model and API endpoint you choose only certain languages are available.
   */
  language?: string;
  /**
   * Spoken measurements will be converted to their corresponding abbreviations.
   */
  measurements?: boolean;
  /**
   * Opts out requests from the Deepgram Model Improvement Program. Refer to our Docs for pricing impacts before setting this to true. https://dpgr.am/deepgram-mip.
   */
  mip_opt_out?: boolean;
  /**
   * Mode of operation for the model representing broad area of topic that will be talked about in the supplied audio
   */
  mode?: "general" | "medical" | "finance";
  /**
   * Transcribe each audio channel independently.
   */
  multichannel?: boolean;
  /**
   * Numerals converts numbers from written format to numerical format.
   */
  numerals?: boolean;
  /**
   * Splits audio into paragraphs to improve transcript readability.
   */
  paragraphs?: boolean;
  /**
   * Profanity Filter looks for recognized profanity and converts it to the nearest recognized non-profane word or removes it from the transcript completely.
   */
  profanity_filter?: boolean;
  /**
   * Add punctuation and capitalization to the transcript.
   */
  punctuate?: boolean;
  /**
   * Redaction removes sensitive information from your transcripts.
   */
  redact?: string;
  /**
   * Search for terms or phrases in submitted audio and replaces them.
   */
  replace?: string;
  /**
   * Search for terms or phrases in submitted audio.
   */
  search?: string;
  /**
   * Recognizes the sentiment throughout a transcript or text.
   */
  sentiment?: boolean;
  /**
   * Apply formatting to transcript output. When set to true, additional formatting will be applied to transcripts to improve readability.
   */
  smart_format?: boolean;
  /**
   * Detect topics throughout a transcript or text.
   */
  topics?: boolean;
  /**
   * Segments speech into meaningful semantic units.
   */
  utterances?: boolean;
  /**
   * Seconds to wait before detecting a pause between words in submitted audio.
   */
  utt_split?: number;
  /**
   * The number of channels in the submitted audio
   */
  channels?: number;
  /**
   * Specifies whether the streaming endpoint should provide ongoing transcription updates as more audio is received. When set to true, the endpoint sends continuous updates, meaning transcription results may evolve over time. Note: Supported only for webosockets.
   */
  interim_results?: boolean;
  /**
   * Indicates how long model will wait to detect whether a speaker has finished speaking or pauses for a significant period of time. When set to a value, the streaming endpoint immediately finalizes the transcription for the processed time range and returns the transcript with a speech_final parameter set to true. Can also be set to false to disable endpointing
   */
  endpointing?: string;
  /**
   * Indicates that speech has started. You'll begin receiving Speech Started messages upon speech starting. Note: Supported only for webosockets.
   */
  vad_events?: boolean;
  /**
   * Indicates how long model will wait to send an UtteranceEnd message after a word has been transcribed. Use with interim_results. Note: Supported only for webosockets.
   */
  utterance_end_ms?: boolean;
}
export interface Ai_Cf_Deepgram_Nova_3_Output {
  results?: {
    channels?: {
      alternatives?: {
        confidence?: number;
        transcript?: string;
        words?: {
          confidence?: number;
          end?: number;
          start?: number;
          word?: string;
        }[];
      }[];
    }[];
    summary?: {
      result?: string;
      short?: string;
    };
    sentiments?: {
      segments?: {
        text?: string;
        start_word?: number;
        end_word?: number;
        sentiment?: string;
        sentiment_score?: number;
      }[];
      average?: {
        sentiment?: string;
        sentiment_score?: number;
      };
    };
  };
}
export declare abstract class Base_Ai_Cf_Deepgram_Nova_3 {
  inputs: Ai_Cf_Deepgram_Nova_3_Input;
  postProcessedOutputs: Ai_Cf_Deepgram_Nova_3_Output;
}
export interface Ai_Cf_Qwen_Qwen3_Embedding_0_6B_Input {
  queries?: string | string[];
  /**
   * Optional instruction for the task
   */
  instruction?: string;
  documents?: string | string[];
  text?: string | string[];
}
export interface Ai_Cf_Qwen_Qwen3_Embedding_0_6B_Output {
  data?: number[][];
  shape?: number[];
}
export declare abstract class Base_Ai_Cf_Qwen_Qwen3_Embedding_0_6B {
  inputs: Ai_Cf_Qwen_Qwen3_Embedding_0_6B_Input;
  postProcessedOutputs: Ai_Cf_Qwen_Qwen3_Embedding_0_6B_Output;
}
export type Ai_Cf_Pipecat_Ai_Smart_Turn_V2_Input =
  | {
      /**
       * readable stream with audio data and content-type specified for that data
       */
      audio: {
        body: object;
        contentType: string;
      };
      /**
       * type of data PCM data that's sent to the inference server as raw array
       */
      dtype?: "uint8" | "float32" | "float64";
    }
  | {
      /**
       * base64 encoded audio data
       */
      audio: string;
      /**
       * type of data PCM data that's sent to the inference server as raw array
       */
      dtype?: "uint8" | "float32" | "float64";
    };
export interface Ai_Cf_Pipecat_Ai_Smart_Turn_V2_Output {
  /**
   * if true, end-of-turn was detected
   */
  is_complete?: boolean;
  /**
   * probability of the end-of-turn detection
   */
  probability?: number;
}
export declare abstract class Base_Ai_Cf_Pipecat_Ai_Smart_Turn_V2 {
  inputs: Ai_Cf_Pipecat_Ai_Smart_Turn_V2_Input;
  postProcessedOutputs: Ai_Cf_Pipecat_Ai_Smart_Turn_V2_Output;
}
export declare abstract class Base_Ai_Cf_Openai_Gpt_Oss_120B {
  inputs: XOR<ResponsesInput, AiTextGenerationInput>;
  postProcessedOutputs: XOR<ResponsesOutput, AiTextGenerationOutput>;
}
export declare abstract class Base_Ai_Cf_Openai_Gpt_Oss_20B {
  inputs: XOR<ResponsesInput, AiTextGenerationInput>;
  postProcessedOutputs: XOR<ResponsesOutput, AiTextGenerationOutput>;
}
export interface Ai_Cf_Leonardo_Phoenix_1_0_Input {
  /**
   * A text description of the image you want to generate.
   */
  prompt: string;
  /**
   * Controls how closely the generated image should adhere to the prompt; higher values make the image more aligned with the prompt
   */
  guidance?: number;
  /**
   * Random seed for reproducibility of the image generation
   */
  seed?: number;
  /**
   * The height of the generated image in pixels
   */
  height?: number;
  /**
   * The width of the generated image in pixels
   */
  width?: number;
  /**
   * The number of diffusion steps; higher values can improve quality but take longer
   */
  num_steps?: number;
  /**
   * Specify what to exclude from the generated images
   */
  negative_prompt?: string;
}
/**
 * The generated image in JPEG format
 */
export type Ai_Cf_Leonardo_Phoenix_1_0_Output = string;
export declare abstract class Base_Ai_Cf_Leonardo_Phoenix_1_0 {
  inputs: Ai_Cf_Leonardo_Phoenix_1_0_Input;
  postProcessedOutputs: Ai_Cf_Leonardo_Phoenix_1_0_Output;
}
export interface Ai_Cf_Leonardo_Lucid_Origin_Input {
  /**
   * A text description of the image you want to generate.
   */
  prompt: string;
  /**
   * Controls how closely the generated image should adhere to the prompt; higher values make the image more aligned with the prompt
   */
  guidance?: number;
  /**
   * Random seed for reproducibility of the image generation
   */
  seed?: number;
  /**
   * The height of the generated image in pixels
   */
  height?: number;
  /**
   * The width of the generated image in pixels
   */
  width?: number;
  /**
   * The number of diffusion steps; higher values can improve quality but take longer
   */
  num_steps?: number;
  /**
   * The number of diffusion steps; higher values can improve quality but take longer
   */
  steps?: number;
}
export interface Ai_Cf_Leonardo_Lucid_Origin_Output {
  /**
   * The generated image in Base64 format.
   */
  image?: string;
}
export declare abstract class Base_Ai_Cf_Leonardo_Lucid_Origin {
  inputs: Ai_Cf_Leonardo_Lucid_Origin_Input;
  postProcessedOutputs: Ai_Cf_Leonardo_Lucid_Origin_Output;
}
export interface Ai_Cf_Deepgram_Aura_1_Input {
  /**
   * Speaker used to produce the audio.
   */
  speaker?:
    | "angus"
    | "asteria"
    | "arcas"
    | "orion"
    | "orpheus"
    | "athena"
    | "luna"
    | "zeus"
    | "perseus"
    | "helios"
    | "hera"
    | "stella";
  /**
   * Encoding of the output audio.
   */
  encoding?: "linear16" | "flac" | "mulaw" | "alaw" | "mp3" | "opus" | "aac";
  /**
   * Container specifies the file format wrapper for the output audio. The available options depend on the encoding type..
   */
  container?: "none" | "wav" | "ogg";
  /**
   * The text content to be converted to speech
   */
  text: string;
  /**
   * Sample Rate specifies the sample rate for the output audio. Based on the encoding, different sample rates are supported. For some encodings, the sample rate is not configurable
   */
  sample_rate?: number;
  /**
   * The bitrate of the audio in bits per second. Choose from predefined ranges or specific values based on the encoding type.
   */
  bit_rate?: number;
}
/**
 * The generated audio in MP3 format
 */
export type Ai_Cf_Deepgram_Aura_1_Output = string;
export declare abstract class Base_Ai_Cf_Deepgram_Aura_1 {
  inputs: Ai_Cf_Deepgram_Aura_1_Input;
  postProcessedOutputs: Ai_Cf_Deepgram_Aura_1_Output;
}
export interface Ai_Cf_Ai4Bharat_Indictrans2_En_Indic_1B_Input {
  /**
   * Input text to translate. Can be a single string or a list of strings.
   */
  text: string | string[];
  /**
   * Target language to translate to
   */
  target_language:
    | "asm_Beng"
    | "awa_Deva"
    | "ben_Beng"
    | "bho_Deva"
    | "brx_Deva"
    | "doi_Deva"
    | "eng_Latn"
    | "gom_Deva"
    | "gon_Deva"
    | "guj_Gujr"
    | "hin_Deva"
    | "hne_Deva"
    | "kan_Knda"
    | "kas_Arab"
    | "kas_Deva"
    | "kha_Latn"
    | "lus_Latn"
    | "mag_Deva"
    | "mai_Deva"
    | "mal_Mlym"
    | "mar_Deva"
    | "mni_Beng"
    | "mni_Mtei"
    | "npi_Deva"
    | "ory_Orya"
    | "pan_Guru"
    | "san_Deva"
    | "sat_Olck"
    | "snd_Arab"
    | "snd_Deva"
    | "tam_Taml"
    | "tel_Telu"
    | "urd_Arab"
    | "unr_Deva";
}
export interface Ai_Cf_Ai4Bharat_Indictrans2_En_Indic_1B_Output {
  /**
   * Translated texts
   */
  translations: string[];
}
export declare abstract class Base_Ai_Cf_Ai4Bharat_Indictrans2_En_Indic_1B {
  inputs: Ai_Cf_Ai4Bharat_Indictrans2_En_Indic_1B_Input;
  postProcessedOutputs: Ai_Cf_Ai4Bharat_Indictrans2_En_Indic_1B_Output;
}
export type Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Input =
  | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Prompt
  | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Messages
  | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Async_Batch;
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Prompt {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * Name of the LoRA (Low-Rank Adaptation) model to fine-tune the base model.
   */
  lora?: string;
  response_format?: Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Messages {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role: string;
    /**
     * The content of the message as a string.
     */
    content: string;
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode_1;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode_1 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Async_Batch {
  requests: (
    | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Prompt_1
    | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Messages_1
  )[];
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Prompt_1 {
  /**
   * The input text prompt for the model to generate a response.
   */
  prompt: string;
  /**
   * Name of the LoRA (Low-Rank Adaptation) model to fine-tune the base model.
   */
  lora?: string;
  response_format?: Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode_2;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode_2 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Messages_1 {
  /**
   * An array of message objects representing the conversation history.
   */
  messages: {
    /**
     * The role of the message sender (e.g., 'user', 'assistant', 'system', 'tool').
     */
    role: string;
    /**
     * The content of the message as a string.
     */
    content: string;
  }[];
  functions?: {
    name: string;
    code: string;
  }[];
  /**
   * A list of tools available for the assistant to use.
   */
  tools?: (
    | {
        /**
         * The name of the tool. More descriptive the better.
         */
        name: string;
        /**
         * A brief description of what the tool does.
         */
        description: string;
        /**
         * Schema defining the parameters accepted by the tool.
         */
        parameters: {
          /**
           * The type of the parameters object (usually 'object').
           */
          type: string;
          /**
           * List of required parameter names.
           */
          required?: string[];
          /**
           * Definitions of each parameter.
           */
          properties: {
            [k: string]: {
              /**
               * The data type of the parameter.
               */
              type: string;
              /**
               * A description of the expected parameter.
               */
              description: string;
            };
          };
        };
      }
    | {
        /**
         * Specifies the type of tool (e.g., 'function').
         */
        type: string;
        /**
         * Details of the function tool.
         */
        function: {
          /**
           * The name of the function.
           */
          name: string;
          /**
           * A brief description of what the function does.
           */
          description: string;
          /**
           * Schema defining the parameters accepted by the function.
           */
          parameters: {
            /**
             * The type of the parameters object (usually 'object').
             */
            type: string;
            /**
             * List of required parameter names.
             */
            required?: string[];
            /**
             * Definitions of each parameter.
             */
            properties: {
              [k: string]: {
                /**
                 * The data type of the parameter.
                 */
                type: string;
                /**
                 * A description of the expected parameter.
                 */
                description: string;
              };
            };
          };
        };
      }
  )[];
  response_format?: Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode_3;
  /**
   * If true, a chat template is not applied and you must adhere to the specific model's expected formatting.
   */
  raw?: boolean;
  /**
   * If true, the response will be streamed back incrementally using SSE, Server Sent Events.
   */
  stream?: boolean;
  /**
   * The maximum number of tokens to generate in the response.
   */
  max_tokens?: number;
  /**
   * Controls the randomness of the output; higher values produce more random results.
   */
  temperature?: number;
  /**
   * Adjusts the creativity of the AI's responses by controlling how many possible words it considers. Lower values make outputs more predictable; higher values allow for more varied and creative responses.
   */
  top_p?: number;
  /**
   * Limits the AI to choose from the top 'k' most probable words. Lower values make responses more focused; higher values introduce more variety and potential surprises.
   */
  top_k?: number;
  /**
   * Random seed for reproducibility of the generation.
   */
  seed?: number;
  /**
   * Penalty for repeated tokens; higher values discourage repetition.
   */
  repetition_penalty?: number;
  /**
   * Decreases the likelihood of the model repeating the same lines verbatim.
   */
  frequency_penalty?: number;
  /**
   * Increases the likelihood of the model introducing new topics.
   */
  presence_penalty?: number;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_JSON_Mode_3 {
  type?: "json_object" | "json_schema";
  json_schema?: unknown;
}
export type Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Output =
  | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Chat_Completion_Response
  | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Text_Completion_Response
  | string
  | Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_AsyncResponse;
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Chat_Completion_Response {
  /**
   * Unique identifier for the completion
   */
  id?: string;
  /**
   * Object type identifier
   */
  object?: "chat.completion";
  /**
   * Unix timestamp of when the completion was created
   */
  created?: number;
  /**
   * Model used for the completion
   */
  model?: string;
  /**
   * List of completion choices
   */
  choices?: {
    /**
     * Index of the choice in the list
     */
    index?: number;
    /**
     * The message generated by the model
     */
    message?: {
      /**
       * Role of the message author
       */
      role: string;
      /**
       * The content of the message
       */
      content: string;
      /**
       * Internal reasoning content (if available)
       */
      reasoning_content?: string;
      /**
       * Tool calls made by the assistant
       */
      tool_calls?: {
        /**
         * Unique identifier for the tool call
         */
        id: string;
        /**
         * Type of tool call
         */
        type: "function";
        function: {
          /**
           * Name of the function to call
           */
          name: string;
          /**
           * JSON string of arguments for the function
           */
          arguments: string;
        };
      }[];
    };
    /**
     * Reason why the model stopped generating
     */
    finish_reason?: string;
    /**
     * Stop reason (may be null)
     */
    stop_reason?: string | null;
    /**
     * Log probabilities (if requested)
     */
    logprobs?: {} | null;
  }[];
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
  /**
   * Log probabilities for the prompt (if requested)
   */
  prompt_logprobs?: {} | null;
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Text_Completion_Response {
  /**
   * Unique identifier for the completion
   */
  id?: string;
  /**
   * Object type identifier
   */
  object?: "text_completion";
  /**
   * Unix timestamp of when the completion was created
   */
  created?: number;
  /**
   * Model used for the completion
   */
  model?: string;
  /**
   * List of completion choices
   */
  choices?: {
    /**
     * Index of the choice in the list
     */
    index: number;
    /**
     * The generated text completion
     */
    text: string;
    /**
     * Reason why the model stopped generating
     */
    finish_reason: string;
    /**
     * Stop reason (may be null)
     */
    stop_reason?: string | null;
    /**
     * Log probabilities (if requested)
     */
    logprobs?: {} | null;
    /**
     * Log probabilities for the prompt (if requested)
     */
    prompt_logprobs?: {} | null;
  }[];
  /**
   * Usage statistics for the inference request
   */
  usage?: {
    /**
     * Total number of tokens in input
     */
    prompt_tokens?: number;
    /**
     * Total number of tokens in output
     */
    completion_tokens?: number;
    /**
     * Total number of input and output tokens
     */
    total_tokens?: number;
  };
}
export interface Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_AsyncResponse {
  /**
   * The async request id that can be used to obtain the results.
   */
  request_id?: string;
}
export declare abstract class Base_Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It {
  inputs: Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Input;
  postProcessedOutputs: Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It_Output;
}
export interface Ai_Cf_Pfnet_Plamo_Embedding_1B_Input {
  /**
   * Input text to embed. Can be a single string or a list of strings.
   */
  text: string | string[];
}
export interface Ai_Cf_Pfnet_Plamo_Embedding_1B_Output {
  /**
   * Embedding vectors, where each vector is a list of floats.
   */
  data: number[][];
  /**
   * Shape of the embedding data as [number_of_embeddings, embedding_dimension].
   *
   * @minItems 2
   * @maxItems 2
   */
  shape: [number, number];
}
export declare abstract class Base_Ai_Cf_Pfnet_Plamo_Embedding_1B {
  inputs: Ai_Cf_Pfnet_Plamo_Embedding_1B_Input;
  postProcessedOutputs: Ai_Cf_Pfnet_Plamo_Embedding_1B_Output;
}
export interface Ai_Cf_Deepgram_Flux_Input {
  /**
   * Encoding of the audio stream. Currently only supports raw signed little-endian 16-bit PCM.
   */
  encoding: "linear16";
  /**
   * Sample rate of the audio stream in Hz.
   */
  sample_rate: string;
  /**
   * End-of-turn confidence required to fire an eager end-of-turn event. When set, enables EagerEndOfTurn and TurnResumed events. Valid Values 0.3 - 0.9.
   */
  eager_eot_threshold?: string;
  /**
   * End-of-turn confidence required to finish a turn. Valid Values 0.5 - 0.9.
   */
  eot_threshold?: string;
  /**
   * A turn will be finished when this much time has passed after speech, regardless of EOT confidence.
   */
  eot_timeout_ms?: string;
  /**
   * Keyterm prompting can improve recognition of specialized terminology. Pass multiple keyterm query parameters to boost multiple keyterms.
   */
  keyterm?: string;
  /**
   * Opts out requests from the Deepgram Model Improvement Program. Refer to Deepgram Docs for pricing impacts before setting this to true. https://dpgr.am/deepgram-mip
   */
  mip_opt_out?: "true" | "false";
  /**
   * Label your requests for the purpose of identification during usage reporting
   */
  tag?: string;
}
/**
 * Output will be returned as websocket messages.
 */
export interface Ai_Cf_Deepgram_Flux_Output {
  /**
   * The unique identifier of the request (uuid)
   */
  request_id?: string;
  /**
   * Starts at 0 and increments for each message the server sends to the client.
   */
  sequence_id?: number;
  /**
   * The type of event being reported.
   */
  event?: "Update" | "StartOfTurn" | "EagerEndOfTurn" | "TurnResumed" | "EndOfTurn";
  /**
   * The index of the current turn
   */
  turn_index?: number;
  /**
   * Start time in seconds of the audio range that was transcribed
   */
  audio_window_start?: number;
  /**
   * End time in seconds of the audio range that was transcribed
   */
  audio_window_end?: number;
  /**
   * Text that was said over the course of the current turn
   */
  transcript?: string;
  /**
   * The words in the transcript
   */
  words?: {
    /**
     * The individual punctuated, properly-cased word from the transcript
     */
    word: string;
    /**
     * Confidence that this word was transcribed correctly
     */
    confidence: number;
  }[];
  /**
   * Confidence that no more speech is coming in this turn
   */
  end_of_turn_confidence?: number;
}
export declare abstract class Base_Ai_Cf_Deepgram_Flux {
  inputs: Ai_Cf_Deepgram_Flux_Input;
  postProcessedOutputs: Ai_Cf_Deepgram_Flux_Output;
}
export interface Ai_Cf_Deepgram_Aura_2_En_Input {
  /**
   * Speaker used to produce the audio.
   */
  speaker?:
    | "amalthea"
    | "andromeda"
    | "apollo"
    | "arcas"
    | "aries"
    | "asteria"
    | "athena"
    | "atlas"
    | "aurora"
    | "callista"
    | "cora"
    | "cordelia"
    | "delia"
    | "draco"
    | "electra"
    | "harmonia"
    | "helena"
    | "hera"
    | "hermes"
    | "hyperion"
    | "iris"
    | "janus"
    | "juno"
    | "jupiter"
    | "luna"
    | "mars"
    | "minerva"
    | "neptune"
    | "odysseus"
    | "ophelia"
    | "orion"
    | "orpheus"
    | "pandora"
    | "phoebe"
    | "pluto"
    | "saturn"
    | "thalia"
    | "theia"
    | "vesta"
    | "zeus";
  /**
   * Encoding of the output audio.
   */
  encoding?: "linear16" | "flac" | "mulaw" | "alaw" | "mp3" | "opus" | "aac";
  /**
   * Container specifies the file format wrapper for the output audio. The available options depend on the encoding type..
   */
  container?: "none" | "wav" | "ogg";
  /**
   * The text content to be converted to speech
   */
  text: string;
  /**
   * Sample Rate specifies the sample rate for the output audio. Based on the encoding, different sample rates are supported. For some encodings, the sample rate is not configurable
   */
  sample_rate?: number;
  /**
   * The bitrate of the audio in bits per second. Choose from predefined ranges or specific values based on the encoding type.
   */
  bit_rate?: number;
}
/**
 * The generated audio in MP3 format
 */
export type Ai_Cf_Deepgram_Aura_2_En_Output = string;
export declare abstract class Base_Ai_Cf_Deepgram_Aura_2_En {
  inputs: Ai_Cf_Deepgram_Aura_2_En_Input;
  postProcessedOutputs: Ai_Cf_Deepgram_Aura_2_En_Output;
}
export interface Ai_Cf_Deepgram_Aura_2_Es_Input {
  /**
   * Speaker used to produce the audio.
   */
  speaker?:
    | "sirio"
    | "nestor"
    | "carina"
    | "celeste"
    | "alvaro"
    | "diana"
    | "aquila"
    | "selena"
    | "estrella"
    | "javier";
  /**
   * Encoding of the output audio.
   */
  encoding?: "linear16" | "flac" | "mulaw" | "alaw" | "mp3" | "opus" | "aac";
  /**
   * Container specifies the file format wrapper for the output audio. The available options depend on the encoding type..
   */
  container?: "none" | "wav" | "ogg";
  /**
   * The text content to be converted to speech
   */
  text: string;
  /**
   * Sample Rate specifies the sample rate for the output audio. Based on the encoding, different sample rates are supported. For some encodings, the sample rate is not configurable
   */
  sample_rate?: number;
  /**
   * The bitrate of the audio in bits per second. Choose from predefined ranges or specific values based on the encoding type.
   */
  bit_rate?: number;
}
/**
 * The generated audio in MP3 format
 */
export type Ai_Cf_Deepgram_Aura_2_Es_Output = string;
export declare abstract class Base_Ai_Cf_Deepgram_Aura_2_Es {
  inputs: Ai_Cf_Deepgram_Aura_2_Es_Input;
  postProcessedOutputs: Ai_Cf_Deepgram_Aura_2_Es_Output;
}
export interface AiModels {
  "@cf/huggingface/distilbert-sst-2-int8": BaseAiTextClassification;
  "@cf/stabilityai/stable-diffusion-xl-base-1.0": BaseAiTextToImage;
  "@cf/runwayml/stable-diffusion-v1-5-inpainting": BaseAiTextToImage;
  "@cf/runwayml/stable-diffusion-v1-5-img2img": BaseAiTextToImage;
  "@cf/lykon/dreamshaper-8-lcm": BaseAiTextToImage;
  "@cf/bytedance/stable-diffusion-xl-lightning": BaseAiTextToImage;
  "@cf/myshell-ai/melotts": BaseAiTextToSpeech;
  "@cf/google/embeddinggemma-300m": BaseAiTextEmbeddings;
  "@cf/microsoft/resnet-50": BaseAiImageClassification;
  "@cf/meta/llama-2-7b-chat-int8": BaseAiTextGeneration;
  "@cf/mistral/mistral-7b-instruct-v0.1": BaseAiTextGeneration;
  "@cf/meta/llama-2-7b-chat-fp16": BaseAiTextGeneration;
  "@hf/thebloke/llama-2-13b-chat-awq": BaseAiTextGeneration;
  "@hf/thebloke/mistral-7b-instruct-v0.1-awq": BaseAiTextGeneration;
  "@hf/thebloke/zephyr-7b-beta-awq": BaseAiTextGeneration;
  "@hf/thebloke/openhermes-2.5-mistral-7b-awq": BaseAiTextGeneration;
  "@hf/thebloke/neural-chat-7b-v3-1-awq": BaseAiTextGeneration;
  "@hf/thebloke/llamaguard-7b-awq": BaseAiTextGeneration;
  "@hf/thebloke/deepseek-coder-6.7b-base-awq": BaseAiTextGeneration;
  "@hf/thebloke/deepseek-coder-6.7b-instruct-awq": BaseAiTextGeneration;
  "@cf/deepseek-ai/deepseek-math-7b-instruct": BaseAiTextGeneration;
  "@cf/defog/sqlcoder-7b-2": BaseAiTextGeneration;
  "@cf/openchat/openchat-3.5-0106": BaseAiTextGeneration;
  "@cf/tiiuae/falcon-7b-instruct": BaseAiTextGeneration;
  "@cf/thebloke/discolm-german-7b-v1-awq": BaseAiTextGeneration;
  "@cf/qwen/qwen1.5-0.5b-chat": BaseAiTextGeneration;
  "@cf/qwen/qwen1.5-7b-chat-awq": BaseAiTextGeneration;
  "@cf/qwen/qwen1.5-14b-chat-awq": BaseAiTextGeneration;
  "@cf/tinyllama/tinyllama-1.1b-chat-v1.0": BaseAiTextGeneration;
  "@cf/microsoft/phi-2": BaseAiTextGeneration;
  "@cf/qwen/qwen1.5-1.8b-chat": BaseAiTextGeneration;
  "@cf/mistral/mistral-7b-instruct-v0.2-lora": BaseAiTextGeneration;
  "@hf/nousresearch/hermes-2-pro-mistral-7b": BaseAiTextGeneration;
  "@hf/nexusflow/starling-lm-7b-beta": BaseAiTextGeneration;
  "@hf/google/gemma-7b-it": BaseAiTextGeneration;
  "@cf/meta-llama/llama-2-7b-chat-hf-lora": BaseAiTextGeneration;
  "@cf/google/gemma-2b-it-lora": BaseAiTextGeneration;
  "@cf/google/gemma-7b-it-lora": BaseAiTextGeneration;
  "@hf/mistral/mistral-7b-instruct-v0.2": BaseAiTextGeneration;
  "@cf/meta/llama-3-8b-instruct": BaseAiTextGeneration;
  "@cf/fblgit/una-cybertron-7b-v2-bf16": BaseAiTextGeneration;
  "@cf/meta/llama-3-8b-instruct-awq": BaseAiTextGeneration;
  "@cf/meta/llama-3.1-8b-instruct-fp8": BaseAiTextGeneration;
  "@cf/meta/llama-3.1-8b-instruct-awq": BaseAiTextGeneration;
  "@cf/meta/llama-3.2-3b-instruct": BaseAiTextGeneration;
  "@cf/meta/llama-3.2-1b-instruct": BaseAiTextGeneration;
  "@cf/deepseek-ai/deepseek-r1-distill-qwen-32b": BaseAiTextGeneration;
  "@cf/ibm-granite/granite-4.0-h-micro": BaseAiTextGeneration;
  "@cf/facebook/bart-large-cnn": BaseAiSummarization;
  "@cf/llava-hf/llava-1.5-7b-hf": BaseAiImageToText;
  "@cf/baai/bge-base-en-v1.5": Base_Ai_Cf_Baai_Bge_Base_En_V1_5;
  "@cf/openai/whisper": Base_Ai_Cf_Openai_Whisper;
  "@cf/meta/m2m100-1.2b": Base_Ai_Cf_Meta_M2M100_1_2B;
  "@cf/baai/bge-small-en-v1.5": Base_Ai_Cf_Baai_Bge_Small_En_V1_5;
  "@cf/baai/bge-large-en-v1.5": Base_Ai_Cf_Baai_Bge_Large_En_V1_5;
  "@cf/unum/uform-gen2-qwen-500m": Base_Ai_Cf_Unum_Uform_Gen2_Qwen_500M;
  "@cf/openai/whisper-tiny-en": Base_Ai_Cf_Openai_Whisper_Tiny_En;
  "@cf/openai/whisper-large-v3-turbo": Base_Ai_Cf_Openai_Whisper_Large_V3_Turbo;
  "@cf/baai/bge-m3": Base_Ai_Cf_Baai_Bge_M3;
  "@cf/black-forest-labs/flux-1-schnell": Base_Ai_Cf_Black_Forest_Labs_Flux_1_Schnell;
  "@cf/meta/llama-3.2-11b-vision-instruct": Base_Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct;
  "@cf/meta/llama-3.3-70b-instruct-fp8-fast": Base_Ai_Cf_Meta_Llama_3_3_70B_Instruct_Fp8_Fast;
  "@cf/meta/llama-guard-3-8b": Base_Ai_Cf_Meta_Llama_Guard_3_8B;
  "@cf/baai/bge-reranker-base": Base_Ai_Cf_Baai_Bge_Reranker_Base;
  "@cf/qwen/qwen2.5-coder-32b-instruct": Base_Ai_Cf_Qwen_Qwen2_5_Coder_32B_Instruct;
  "@cf/qwen/qwq-32b": Base_Ai_Cf_Qwen_Qwq_32B;
  "@cf/mistralai/mistral-small-3.1-24b-instruct": Base_Ai_Cf_Mistralai_Mistral_Small_3_1_24B_Instruct;
  "@cf/google/gemma-3-12b-it": Base_Ai_Cf_Google_Gemma_3_12B_It;
  "@cf/meta/llama-4-scout-17b-16e-instruct": Base_Ai_Cf_Meta_Llama_4_Scout_17B_16E_Instruct;
  "@cf/qwen/qwen3-30b-a3b-fp8": Base_Ai_Cf_Qwen_Qwen3_30B_A3B_Fp8;
  "@cf/deepgram/nova-3": Base_Ai_Cf_Deepgram_Nova_3;
  "@cf/qwen/qwen3-embedding-0.6b": Base_Ai_Cf_Qwen_Qwen3_Embedding_0_6B;
  "@cf/pipecat-ai/smart-turn-v2": Base_Ai_Cf_Pipecat_Ai_Smart_Turn_V2;
  "@cf/openai/gpt-oss-120b": Base_Ai_Cf_Openai_Gpt_Oss_120B;
  "@cf/openai/gpt-oss-20b": Base_Ai_Cf_Openai_Gpt_Oss_20B;
  "@cf/leonardo/phoenix-1.0": Base_Ai_Cf_Leonardo_Phoenix_1_0;
  "@cf/leonardo/lucid-origin": Base_Ai_Cf_Leonardo_Lucid_Origin;
  "@cf/deepgram/aura-1": Base_Ai_Cf_Deepgram_Aura_1;
  "@cf/ai4bharat/indictrans2-en-indic-1B": Base_Ai_Cf_Ai4Bharat_Indictrans2_En_Indic_1B;
  "@cf/aisingapore/gemma-sea-lion-v4-27b-it": Base_Ai_Cf_Aisingapore_Gemma_Sea_Lion_V4_27B_It;
  "@cf/pfnet/plamo-embedding-1b": Base_Ai_Cf_Pfnet_Plamo_Embedding_1B;
  "@cf/deepgram/flux": Base_Ai_Cf_Deepgram_Flux;
  "@cf/deepgram/aura-2-en": Base_Ai_Cf_Deepgram_Aura_2_En;
  "@cf/deepgram/aura-2-es": Base_Ai_Cf_Deepgram_Aura_2_Es;
}
export type AiOptions = {
  /**
   * Send requests as an asynchronous batch job, only works for supported models
   * https://developers.cloudflare.com/workers-ai/features/batch-api
   */
  queueRequest?: boolean;
  /**
   * Establish websocket connections, only works for supported models
   */
  websocket?: boolean;
  /**
   * Tag your requests to group and view them in Cloudflare dashboard.
   *
   * Rules:
   * Tags must only contain letters, numbers, and the symbols: : - . / @
   * Each tag can have maximum 50 characters.
   * Maximum 5 tags are allowed each request.
   * Duplicate tags will removed.
   */
  tags?: string[];
  gateway?: GatewayOptions;
  returnRawResponse?: boolean;
  prefix?: string;
  extraHeaders?: object;
};
export type AiModelsSearchParams = {
  author?: string;
  hide_experimental?: boolean;
  page?: number;
  per_page?: number;
  search?: string;
  source?: number;
  task?: string;
};
export type AiModelsSearchObject = {
  id: string;
  source: number;
  name: string;
  description: string;
  task: {
    id: string;
    name: string;
    description: string;
  };
  tags: string[];
  properties: {
    property_id: string;
    value: string;
  }[];
};
export interface InferenceUpstreamError extends Error {}
export interface AiInternalError extends Error {}
export type AiModelListType = Record<string, any>;
export declare abstract class Ai<AiModelList extends AiModelListType = AiModels> {
  aiGatewayLogId: string | null;
  gateway(gatewayId: string): AiGateway;
  autorag(autoragId: string): AutoRAG;
  run<Name extends keyof AiModelList, Options extends AiOptions, InputOptions extends AiModelList[Name]["inputs"]>(
    model: Name,
    inputs: InputOptions,
    options?: Options,
  ): Promise<
    Options extends
      | {
          returnRawResponse: true;
        }
      | {
          websocket: true;
        }
      ? Response
      : InputOptions extends {
            stream: true;
          }
        ? ReadableStream
        : AiModelList[Name]["postProcessedOutputs"]
  >;
  models(params?: AiModelsSearchParams): Promise<AiModelsSearchObject[]>;
  toMarkdown(): ToMarkdownService;
  toMarkdown(
    files: MarkdownDocument[],
    options?: ConversionRequestOptions,
  ): Promise<ConversionResponse[]>;
  toMarkdown(
    files: MarkdownDocument,
    options?: ConversionRequestOptions,
  ): Promise<ConversionResponse>;
}
