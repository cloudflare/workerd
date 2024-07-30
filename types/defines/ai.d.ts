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
export type AiSpeechRecognitionInput = {
  audio: number[];
};
export type AiSpeechRecognitionOutput = {
  text?: string;
  words?: {
    word: string;
    start: number;
    end: number;
  }[];
  vtt?: string;
};
export declare abstract class BaseAiSpeechRecognition {
  inputs: AiSpeechRecognitionInput;
  postProcessedOutputs: AiSpeechRecognitionOutput;
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
  role: "user" | "assistant" | "system" | "tool";
  content: string;
};
export type AiTextGenerationToolInput = {
  type: "function";
  function: {
    name: string;
    description: string;
    parameters?: {
      type: "object";
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
  tools?: AiTextGenerationToolInput[];
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
export type AiTextToImageInput = {
  prompt: string;
  image?: number[];
  mask?: number[];
  num_steps?: number;
  strength?: number;
  guidance?: number;
};
export type AiTextToImageOutput = Uint8Array;
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
export type GatewayOptions = {
  id: string;
  cacheTtl?: number;
  skipCache?: boolean;
  metadata?: Record<string, number | string | boolean | null | bigint>;
};
export type AiOptions = {
  gateway?: GatewayOptions;
  prefix?: string;
  extraHeaders?: object;
};
export type BaseAiTextClassificationModels =
  "@cf/huggingface/distilbert-sst-2-int8";
export type BaseAiTextToImageModels =
  | "@cf/stabilityai/stable-diffusion-xl-base-1.0"
  | "@cf/runwayml/stable-diffusion-v1-5-inpainting"
  | "@cf/runwayml/stable-diffusion-v1-5-img2img"
  | "@cf/lykon/dreamshaper-8-lcm"
  | "@cf/bytedance/stable-diffusion-xl-lightning";
export type BaseAiTextEmbeddingsModels =
  | "@cf/baai/bge-small-en-v1.5"
  | "@cf/baai/bge-base-en-v1.5"
  | "@cf/baai/bge-large-en-v1.5";
export type BaseAiSpeechRecognitionModels =
  | "@cf/openai/whisper"
  | "@cf/openai/whisper-tiny-en"
  | "@cf/openai/whisper-sherpa";
export type BaseAiImageClassificationModels = "@cf/microsoft/resnet-50";
export type BaseAiObjectDetectionModels = "@cf/facebook/detr-resnet-50";
export type BaseAiTextGenerationModels =
  | "@cf/meta/llama-3.1-8b-instruct"
  | "@cf/meta/llama-3-8b-instruct"
  | "@cf/meta/llama-3-8b-instruct-awq"
  | "@cf/meta/llama-2-7b-chat-int8"
  | "@cf/mistral/mistral-7b-instruct-v0.1"
  | "@cf/mistral/mistral-7b-instruct-v0.2-lora"
  | "@cf/meta/llama-2-7b-chat-fp16"
  | "@hf/thebloke/llama-2-13b-chat-awq"
  | "@hf/thebloke/zephyr-7b-beta-awq"
  | "@hf/thebloke/mistral-7b-instruct-v0.1-awq"
  | "@hf/thebloke/codellama-7b-instruct-awq"
  | "@hf/thebloke/openhermes-2.5-mistral-7b-awq"
  | "@hf/thebloke/neural-chat-7b-v3-1-awq"
  | "@hf/thebloke/llamaguard-7b-awq"
  | "@hf/thebloke/deepseek-coder-6.7b-base-awq"
  | "@hf/thebloke/deepseek-coder-6.7b-instruct-awq"
  | "@hf/nousresearch/hermes-2-pro-mistral-7b"
  | "@hf/mistral/mistral-7b-instruct-v0.2"
  | "@hf/google/gemma-7b-it"
  | "@hf/nexusflow/starling-lm-7b-beta"
  | "@cf/deepseek-ai/deepseek-math-7b-instruct"
  | "@cf/defog/sqlcoder-7b-2"
  | "@cf/openchat/openchat-3.5-0106"
  | "@cf/tiiuae/falcon-7b-instruct"
  | "@cf/thebloke/discolm-german-7b-v1-awq"
  | "@cf/qwen/qwen1.5-0.5b-chat"
  | "@cf/qwen/qwen1.5-1.8b-chat"
  | "@cf/qwen/qwen1.5-7b-chat-awq"
  | "@cf/qwen/qwen1.5-14b-chat-awq"
  | "@cf/tinyllama/tinyllama-1.1b-chat-v1.0"
  | "@cf/microsoft/phi-2"
  | "@cf/google/gemma-2b-it-lora"
  | "@cf/google/gemma-7b-it-lora"
  | "@cf/meta-llama/llama-2-7b-chat-hf-lora"
  | "@cf/fblgit/una-cybertron-7b-v2-bf16"
  | "@cf/fblgit/una-cybertron-7b-v2-awq";
export type BaseAiTranslationModels = "@cf/meta/m2m100-1.2b";
export type BaseAiSummarizationModels = "@cf/facebook/bart-large-cnn";
export type BaseAiImageToTextModels =
  | "@cf/unum/uform-gen2-qwen-500m"
  | "@cf/llava-hf/llava-1.5-7b-hf";
export declare abstract class Ai {
  run(
    model: BaseAiTextClassificationModels,
    inputs: BaseAiTextClassification["inputs"],
    options?: AiOptions
  ): Promise<BaseAiTextClassification["postProcessedOutputs"]>;
  run(
    model: BaseAiTextToImageModels,
    inputs: BaseAiTextToImage["inputs"],
    options?: AiOptions
  ): Promise<BaseAiTextToImage["postProcessedOutputs"]>;
  run(
    model: BaseAiTextEmbeddingsModels,
    inputs: BaseAiTextEmbeddings["inputs"],
    options?: AiOptions
  ): Promise<BaseAiTextEmbeddings["postProcessedOutputs"]>;
  run(
    model: BaseAiSpeechRecognitionModels,
    inputs: BaseAiSpeechRecognition["inputs"],
    options?: AiOptions
  ): Promise<BaseAiSpeechRecognition["postProcessedOutputs"]>;
  run(
    model: BaseAiImageClassificationModels,
    inputs: BaseAiImageClassification["inputs"],
    options?: AiOptions
  ): Promise<BaseAiImageClassification["postProcessedOutputs"]>;
  run(
    model: BaseAiObjectDetectionModels,
    inputs: BaseAiObjectDetection["inputs"],
    options?: AiOptions
  ): Promise<BaseAiObjectDetection["postProcessedOutputs"]>;
  run(
    model: BaseAiTextGenerationModels,
    inputs: BaseAiTextGeneration["inputs"],
    options?: AiOptions
  ): Promise<BaseAiTextGeneration["postProcessedOutputs"]>;
  run(
    model: BaseAiTranslationModels,
    inputs: BaseAiTranslation["inputs"],
    options?: AiOptions
  ): Promise<BaseAiTranslation["postProcessedOutputs"]>;
  run(
    model: BaseAiSummarizationModels,
    inputs: BaseAiSummarization["inputs"],
    options?: AiOptions
  ): Promise<BaseAiSummarization["postProcessedOutputs"]>;
  run(
    model: BaseAiImageToTextModels,
    inputs: BaseAiImageToText["inputs"],
    options?: AiOptions
  ): Promise<BaseAiImageToText["postProcessedOutputs"]>;
}
