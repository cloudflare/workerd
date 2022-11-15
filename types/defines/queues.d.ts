/**
 * A message that is sent to a consumer Worker.
 */
interface Message<Body = unknown> {
  /**
   * A unique, system-generated ID for the message.
   */
  readonly id: string;
  /**
   * A timestamp when the message was sent.
   */
  readonly timestamp: Date;
  /**
   * The body of the message.
   */
  readonly body: Body;
}

/**
 * A batch of messages that are sent to a consumer Worker.
 */
interface MessageBatch<Body = unknown> {
  /**
   * The name of the Queue that belongs to this batch.
   */
  readonly queue: string;
  /**
   * An array of messages in the batch. Ordering of messages is not guaranteed.
   */
  readonly messages: readonly Message<Body>[];
  /**
   * Marks every message to be retried in the next batch.
   */
  retryAll(): void;
}

/**
 * A binding that allows a producer to send messages to a Queue.
 */
interface Queue<Body = any> {
  /**
   * Sends a message to the Queue.
   * @param message The message can be any type supported by the [structured clone algorithm](https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Structured_clone_algorithm#supported_types), as long as its size is less than 128 KB.
   * @returns A promise that resolves when the message is confirmed to be written to disk.
   */
  send(message: Body): Promise<void>;
}
