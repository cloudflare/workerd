import {
  serialize,
  deserialize,
  Serializer,
  Deserializer,
} from 'node:v8'

import { Buffer } from 'node:buffer';

import {
  deepStrictEqual,
  strictEqual,
  ok,
} from 'node:assert';

export default {
  async fetch(request) {
    // Basic serialize/deserialize
    {
      const original = { a: 1, b: 2, c: 1n };
      const serialized = serialize(original);
      const deserialized = deserialize(serialized);
      deepStrictEqual(original, deserialized);
    }

    {
      // When using the default serializer, Buffers are automatically handled.
      // However, they are always passed by copy and not transfered.
      const original = Buffer.from('hello');
      const serialized = serialize(original);
      const deserialized = deserialize(serialized);
      strictEqual(original.toString(), deserialized.toString());
    }

    {
      // Using Serializer and Deserializer directly
      const ser = new Serializer();
      ser.writeHeader();
      ser.writeValue(1);
      ser.writeUint32(2);
      ser.writeUint64(1, 2);
      ser.writeDouble(3.1);
      const buf = Buffer.from('hello');
      const u8 = new Uint8Array(10);
      ser.transferArrayBuffer(0, u8.buffer);
      ser.writeValue(buf);
      ser.writeValue(u8);

      const des = new Deserializer(ser.releaseBuffer());
      des.transferArrayBuffer(0, u8.buffer);

      ok(des.readHeader());
      strictEqual(des.readValue(), 1);
      strictEqual(des.readUint32(), 2);
      deepStrictEqual(des.readUint64(), [1, 2]);
      strictEqual(des.readDouble(), 3.1);
      // Because we're not using the default serializer, we need to manually
      // convert the next read value into a Buffer since the default serializer
      // is going to look at it strictly as a Uint8Array.
      strictEqual(Buffer.from(des.readValue().buffer).toString(), 'hello');

      strictEqual(des.readValue().buffer, u8.buffer);

      // The deserializer should throw an error if there are no more values.
      try {
        des.readValue();
        throw new Error('should have thrown');
      } catch (err) {
        ok(err.message.startsWith('Unable to deserialize'));
      }
    }

    return new Response("ok");
  }
};
