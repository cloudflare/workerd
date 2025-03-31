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
export type AiTextGenerationOutput =
  | {
      response?: string;
      tool_calls?: {
        name: string;
        arguments: unknown;
      }[];
    }
  | ReadableStream;
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
  vad_filter?: string;
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
export type Ai_Cf_Baai_Bge_M3_Input = BGEM3InputQueryAndContexts | BGEM3InputEmbedding;
export interface BGEM3InputQueryAndContexts {
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
export interface BGEM3InputEmbedding {
  text: string | string[];
  /**
   * When provided with too long context should the model error out or truncate the context to fit?
   */
  truncate_inputs?: boolean;
}
export type Ai_Cf_Baai_Bge_M3_Output = BGEM3OuputQuery | BGEM3OutputEmbeddingForContexts | BGEM3OuputEmbedding;
export interface BGEM3OuputQuery {
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
export interface BGEM3OutputEmbeddingForContexts {
  response?: number[][];
  shape?: number[];
  /**
   * The pooling method used in the embedding process.
   */
  pooling?: "mean" | "cls";
}
export interface BGEM3OuputEmbedding {
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
export type Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Input = Prompt | Messages;
export interface Prompt {
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
export interface Messages {
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
  image?: number[] | string;
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
export type Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Output =
  | {
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
    }
  | ReadableStream;
export declare abstract class Base_Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct {
  inputs: Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Input;
  postProcessedOutputs: Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct_Output;
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
export interface AiModels {
  "@cf/huggingface/distilbert-sst-2-int8": BaseAiTextClassification;
  "@cf/stabilityai/stable-diffusion-xl-base-1.0": BaseAiTextToImage;
  "@cf/runwayml/stable-diffusion-v1-5-inpainting": BaseAiTextToImage;
  "@cf/runwayml/stable-diffusion-v1-5-img2img": BaseAiTextToImage;
  "@cf/lykon/dreamshaper-8-lcm": BaseAiTextToImage;
  "@cf/bytedance/stable-diffusion-xl-lightning": BaseAiTextToImage;
  "@cf/myshell-ai/melotts": BaseAiTextToSpeech;
  "@cf/baai/bge-base-en-v1.5": BaseAiTextEmbeddings;
  "@cf/baai/bge-small-en-v1.5": BaseAiTextEmbeddings;
  "@cf/baai/bge-large-en-v1.5": BaseAiTextEmbeddings;
  "@cf/microsoft/resnet-50": BaseAiImageClassification;
  "@cf/facebook/detr-resnet-50": BaseAiObjectDetection;
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
  "@hf/meta-llama/meta-llama-3-8b-instruct": BaseAiTextGeneration;
  "@cf/meta/llama-3.1-8b-instruct": BaseAiTextGeneration;
  "@cf/meta/llama-3.1-8b-instruct-fp8": BaseAiTextGeneration;
  "@cf/meta/llama-3.1-8b-instruct-awq": BaseAiTextGeneration;
  "@cf/meta/llama-3.2-3b-instruct": BaseAiTextGeneration;
  "@cf/meta/llama-3.2-1b-instruct": BaseAiTextGeneration;
  "@cf/meta/llama-3.3-70b-instruct-fp8-fast": BaseAiTextGeneration;
  "@cf/deepseek-ai/deepseek-r1-distill-qwen-32b": BaseAiTextGeneration;
  "@cf/meta/m2m100-1.2b": BaseAiTranslation;
  "@cf/facebook/bart-large-cnn": BaseAiSummarization;
  "@cf/llava-hf/llava-1.5-7b-hf": BaseAiImageToText;
  "@cf/openai/whisper": Base_Ai_Cf_Openai_Whisper;
  "@cf/unum/uform-gen2-qwen-500m": Base_Ai_Cf_Unum_Uform_Gen2_Qwen_500M;
  "@cf/openai/whisper-tiny-en": Base_Ai_Cf_Openai_Whisper_Tiny_En;
  "@cf/openai/whisper-large-v3-turbo": Base_Ai_Cf_Openai_Whisper_Large_V3_Turbo;
  "@cf/baai/bge-m3": Base_Ai_Cf_Baai_Bge_M3;
  "@cf/black-forest-labs/flux-1-schnell": Base_Ai_Cf_Black_Forest_Labs_Flux_1_Schnell;
  "@cf/meta/llama-3.2-11b-vision-instruct": Base_Ai_Cf_Meta_Llama_3_2_11B_Vision_Instruct;
  "@cf/meta/llama-guard-3-8b": Base_Ai_Cf_Meta_Llama_Guard_3_8B;
  "@cf/baai/bge-reranker-base": Base_Ai_Cf_Baai_Bge_Reranker_Base;
}
export type AiOptions = {
  gateway?: GatewayOptions;
  returnRawResponse?: boolean;
  prefix?: string;
  extraHeaders?: object;
};
export type ConversionResponse = {
  name: string;
  mimeType: string;
  format: "markdown";
  tokens: number;
  data: string;
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
  run<Name extends keyof AiModelList, Options extends AiOptions>(
    model: Name,
    inputs: AiModelList[Name]["inputs"],
    options?: Options,
  ): Promise<
    Options extends {
      returnRawResponse: true;
    }
      ? Response
      : AiModelList[Name]["postProcessedOutputs"]
  >;
  models(params?: AiModelsSearchParams): Promise<AiModelsSearchObject[]>;
  toMarkdown(
    files: {
      name: string;
      blob: Blob;
    }[],
    options?: {
      gateway?: GatewayOptions;
      extraHeaders?: object;
    },
  ): Promise<ConversionResponse[]>;
  toMarkdown(
    files: {
      name: string;
      blob: Blob;
    },
    options?: {
      gateway?: GatewayOptions;
      extraHeaders?: object;
    },
  ): Promise<ConversionResponse>;
}
