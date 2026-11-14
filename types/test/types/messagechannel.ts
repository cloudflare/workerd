// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

const channel = new MessageChannel();
expectType<MessagePort>(channel.port1);
expectType<MessagePort>(channel.port2);
// @ts-expect-error the ports are read-only.
channel.port1 = channel.port2;

const port = channel.port1;
expectType<EventTarget>(port);
port.postMessage('hello');
port.postMessage({ a: 1 }, []);
port.postMessage(undefined, { transfer: [] });
port.start();
port.close();
port.onmessage = (event: MessageEvent) => event.data;
port.onmessage = null;

// @ts-expect-error MessagePort is not constructible.
new MessagePort();

expectType<typeof MessageChannel>(self.MessageChannel);
expectType<typeof MessagePort>(self.MessagePort);

const event = new MessageEvent('message', {
  data: 1,
  source: port,
  ports: [channel.port2],
});
expectType<MessagePort | null>(event.source);
expectType<MessagePort[]>(event.ports);
// @ts-expect-error a source is a MessagePort.
new MessageEvent('message', { source: {} });
