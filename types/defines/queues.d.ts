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
  /**
   * Marks message to be retried.
   */
  retry(): void;
  /**
   * Marks message acknowledged.
   */
  ack(): void;
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
  /**
   * Marks every message acknowledged in the batch.
   */
  ackAll(): void;
}
