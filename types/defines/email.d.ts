/**
 * The returned data after sending an email
 */
interface EmailSendResult {
  /**
   * The Email Message ID
   */
  messageId: string;
}

/**
 * An email message that can be sent from a Worker.
 */
interface EmailMessage {
  /**
   * Envelope From attribute of the email message.
   */
  readonly from: string;
  /**
   * Envelope To attribute of the email message.
   */
  readonly to: string;
}

/**
 * An email message that is sent to a consumer Worker and can be rejected/forwarded.
 */
interface ForwardableEmailMessage extends EmailMessage {
  /**
   * Stream of the email message content.
   */
  readonly raw: ReadableStream<Uint8Array>;
  /**
   * An [Headers object](https://developer.mozilla.org/en-US/docs/Web/API/Headers).
   */
  readonly headers: Headers;
  /**
   * Size of the email message content.
   */
  readonly rawSize: number;
  /**
   * Reject this email message by returning a permanent SMTP error back to the connecting client including the given reason.
   * @param reason The reject reason.
   * @returns void
   */
  setReject(reason: string): void;
  /**
   * Forward this email message to a verified destination address of the account.
   * @param rcptTo Verified destination address.
   * @param headers A [Headers object](https://developer.mozilla.org/en-US/docs/Web/API/Headers).
   * @returns A promise that resolves when the email message is forwarded.
   */
  forward(rcptTo: string, headers?: Headers): Promise<EmailSendResult>;
  /**
   * Reply to the sender of this email message with a new EmailMessage object.
   * @param message The reply message.
   * @returns A promise that resolves when the email message is replied.
   */
  reply(message: EmailMessage): Promise<EmailSendResult>;
  /**
   * Reply to the sender of this email message with a message built from the given
   * fields. Threading headers (In-Reply-To/References) are set automatically.
   * @param builder The reply message contents.
   * @returns A promise that resolves when the email message is replied.
   */
  reply(builder: EmailReplyMessageBuilder): Promise<EmailSendResult>;
}

/** A file attachment for an email message */
type EmailAttachment =
	| { disposition: 'inline'; contentId: string; filename: string; type: string; content: string | ArrayBuffer | ArrayBufferView }
	| { disposition: 'attachment'; contentId?: undefined; filename: string; type: string; content: string | ArrayBuffer | ArrayBufferView };

/** An Email Address */
interface EmailAddress {
	name: string;
	email: string;
}

/**
 * Recipient fields for `SendEmail.send()`. At least one of `to`, `cc`, or
 * `bcc` must be provided.
 */
type EmailDestinations = {
	to?: string | EmailAddress | (string | EmailAddress)[];
	cc?: string | EmailAddress | (string | EmailAddress)[];
	bcc?: string | EmailAddress | (string | EmailAddress)[];
} & (
	| { to: string | EmailAddress | (string | EmailAddress)[] }
	| { cc: string | EmailAddress | (string | EmailAddress)[] }
	| { bcc: string | EmailAddress | (string | EmailAddress)[] }
);

/**
 * Fields shared by all composed emails (no recipients). Used directly by
 * `ForwardableEmailMessage.reply()`, which always replies to the original
 * sender, and extended by `EmailMessageBuilder` for `SendEmail.send()`.
 */
interface EmailReplyMessageBuilder {
	from: string | EmailAddress;
	subject: string;
	replyTo?: string | EmailAddress;
	headers?: Record<string, string>;
	text?: string;
	html?: string;
	attachments?: EmailAttachment[];
}

/**
 * Fields for composing an email without constructing raw MIME, for
 * `SendEmail.send()`. Requires at least one of `to`, `cc`, or `bcc`.
 */
type EmailMessageBuilder = EmailReplyMessageBuilder & EmailDestinations;

/**
 * A binding that allows a Worker to send email messages.
 */
interface SendEmail {
  send(message: EmailMessage): Promise<EmailSendResult>;
  send(builder: EmailMessageBuilder): Promise<EmailSendResult>;
}

declare abstract class EmailEvent extends ExtendableEvent {
  readonly message: ForwardableEmailMessage;
}

declare type EmailExportedHandler<Env = unknown, Props = unknown> = (
  message: ForwardableEmailMessage,
  env: Env,
  ctx: ExecutionContext<Props>
) => void | Promise<void>;

declare module "cloudflare:email" {
  let _EmailMessage: {
    prototype: EmailMessage;
    new (from: string, to: string, raw: ReadableStream | string): EmailMessage;
  };
  export { _EmailMessage as EmailMessage };
}
