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
  role:
    | "user"
    | "assistant"
    | "system"
    | "tool"
    | (string & NonNullable<unknown>);
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
  tools?: AiTextGenerationToolInput[] | AiTextGenerationToolLegacyInput[];
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
export type AiOptions = {
  gateway?: GatewayOptions;
  prefix?: string;
  extraHeaders?: object;
};
export type ModelType<Name extends keyof AiModels> = AiModels[Name];
export interface AiModels {
  "@cf/huggingface/distilbert-sst-2-int8": BaseAiTextClassification;
  "@cf/stabilityai/stable-diffusion-xl-base-1.0": BaseAiTextToImage;
  "@cf/runwayml/stable-diffusion-v1-5-inpainting": BaseAiTextToImage;
  "@cf/runwayml/stable-diffusion-v1-5-img2img": BaseAiTextToImage;
  "@cf/lykon/dreamshaper-8-lcm": BaseAiTextToImage;
  "@cf/bytedance/stable-diffusion-xl-lightning": BaseAiTextToImage;
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
  "@cf/meta/m2m100-1.2b": BaseAiTranslation;
  "@cf/facebook/bart-large-cnn": BaseAiSummarization;
  "@cf/unum/uform-gen2-qwen-500m": BaseAiImageToText;
  "@cf/llava-hf/llava-1.5-7b-hf": BaseAiImageToText;
}
export type ModelListType = Record<string, any>;
export declare abstract class Ai<ModelList extends ModelListType = AiModels> {
  aiGatewayLogId: string | null;
  gateway(gatewayId: string): AiGateway;
  run<Name extends keyof ModelList>(
    model: Name,
    inputs: ModelList[Name]["inputs"],
    options?: AiOptions
  ): Promise<ModelList[Name]["postProcessedOutputs"]>;
}
