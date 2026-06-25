// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

declare const sender: SendEmail;
declare const message: ForwardableEmailMessage;
declare const address: EmailAddress;

// EMAIL-1387: reply() accepts a builder (no recipients — a reply always goes
// back to the original sender).
expectType<Promise<EmailSendResult>>(
  message.reply({ from: 'sender@example.com', subject: 'Re: hello' })
);
// `from` and `replyTo` accept an EmailAddress object (name support).
message.reply({ from: address, subject: 'Re: hello', replyTo: address });

// The reply builder must not carry recipient fields.
// @ts-expect-error reply() does not accept `to`
message.reply({
  from: 'sender@example.com',
  subject: 'Re: hello',
  to: 'recipient@example.com',
});

// reply() still accepts a raw EmailMessage.
declare const raw: EmailMessage;
expectType<Promise<EmailSendResult>>(message.reply(raw));

// A plain send() with a single `to` recipient.
expectType<Promise<EmailSendResult>>(
  sender.send({ from: 'sender@example.com', to: 'recipient@example.com', subject: 'hi' })
);

// EMAIL-1843: `to` is optional when `cc` or `bcc` is provided.
sender.send({ from: 'sender@example.com', cc: 'cc@example.com', subject: 'hi' });
sender.send({ from: 'sender@example.com', bcc: ['bcc@example.com'], subject: 'hi' });

// EMAIL-1692: `name` support on to/cc/bcc via EmailAddress objects and mixed
// arrays, as well as on `from`.
sender.send({ from: address, to: address, subject: 'hi' });
sender.send({
  from: 'sender@example.com',
  cc: [address, 'cc@example.com'],
  subject: 'hi',
});

// send() requires at least one of to/cc/bcc.
// @ts-expect-error send() requires at least one recipient
sender.send({ from: 'sender@example.com', subject: 'hi' });
