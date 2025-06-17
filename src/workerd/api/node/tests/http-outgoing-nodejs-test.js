import { OutgoingMessage } from 'node:http';
import { throws, strictEqual } from 'node:assert';

// This is a modified test which is taken from
// https://github.com/nodejs/node/blob/462c74181d8e15e74bc5a25d55290d93bd7edf65/test/parallel/test-http-outgoing-proto.js#L47
export const testHttpOutgoingProto = {
  async test() {
    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader();
      },
      {
        code: 'ERR_INVALID_HTTP_TOKEN',
        name: 'TypeError',
        message: 'Header name must be a valid HTTP token ["undefined"]',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader('test');
      },
      {
        code: 'ERR_HTTP_INVALID_HEADER_VALUE',
        name: 'TypeError',
        message: 'Invalid value "undefined" for header "test"',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader(404);
      },
      {
        code: 'ERR_INVALID_HTTP_TOKEN',
        name: 'TypeError',
        message: 'Header name must be a valid HTTP token ["404"]',
      }
    );

    throws(
      () => {
        const outgoingMessage = new OutgoingMessage();
        outgoingMessage.setHeader('200', 'あ');
      },
      {
        code: 'ERR_INVALID_CHAR',
        name: 'TypeError',
        message: 'Invalid character in header content ["200"]',
      }
    );

    {
      const outgoingMessage = new OutgoingMessage();
      strictEqual(outgoingMessage.destroyed, false);
      outgoingMessage.destroy();
      strictEqual(outgoingMessage.destroyed, true);
    }
  },
};
