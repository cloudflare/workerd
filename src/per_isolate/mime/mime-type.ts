'use strict';

// A leaf. A parsed MIME type for the bootstrap's API implementations. Parsing
// and serializing are native (utils.mimeTypeParse() and mimeTypeSerialize(),
// over workerd/util/mimetype.h), so a MIME type parses here as it does in the
// C++ that reads Content-Type values.

const { ObjectSetPrototypeOf } = primordials;
const { mimeTypeParse, mimeTypeSerialize } = utils;

class MimeType {
  readonly type: string;
  readonly subtype: string;
  // Names and values: the name of parameter `i` at `2 * i`, its value next.
  readonly #params: string[];

  private constructor(type: string, subtype: string, params: string[]) {
    this.type = type;
    this.subtype = subtype;
    this.#params = params;
  }

  // The MIME type `input` parses as, or undefined.
  static tryParse(input: string): MimeType | undefined {
    const parts = mimeTypeParse(input);
    if (parts === undefined) return undefined;
    // Copied by index into an array with no prototype: Array.prototype.slice()
    // would look up a constructor and Symbol.species that user code can
    // replace.
    const params = ObjectSetPrototypeOf([], null) as string[];
    for (let i = 2; i < parts.length; i++) {
      params[i - 2] = parts[i] as string;
    }
    return new MimeType(parts[0] as string, parts[1] as string, params);
  }

  // `type/subtype`, without the parameters.
  get essence(): string {
    return `${this.type}/${this.subtype}`;
  }

  // The value of the parameter named `name`, which must be lowercase, or
  // undefined.
  param(name: string): string | undefined {
    const params = this.#params;
    for (let i = 0; i < params.length; i += 2) {
      if (params[i] === name) return params[i + 1];
    }
    return undefined;
  }

  toString(): string {
    return mimeTypeSerialize(this.type, this.subtype, this.#params);
  }
}

export type MimeTypeModule = {
  MimeType: typeof MimeType;
};

module.exports = { MimeType };
