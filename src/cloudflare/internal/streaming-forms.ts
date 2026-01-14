const CRLF = '\r\n'

function isReadableStream(obj: unknown): obj is ReadableStream {
  return !!(
    obj &&
    typeof obj === 'object' &&
    'getReader' in obj &&
    typeof obj.getReader === 'function'
  )
}

function chainStreams<T>(streams: ReadableStream<T>[]): ReadableStream<T> {
  const outputStream = new ReadableStream<T>({
    async start(controller): Promise<void> {
      for (const stream of streams) {
        const reader = stream.getReader()

        try {
          // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
          while (true) {
            const { done, value } = await reader.read()
            if (done) break
            if (value !== undefined) controller.enqueue(value)
          }
        } finally {
          reader.releaseLock()
        }
      }

      controller.close()
    },
  })

  return outputStream
}

type EntryOptions = { type: 'file' | 'string' }

export class StreamableFormData {
  #entries: {
    field: string
    value: ReadableStream
    options: EntryOptions
  }[]
  #boundary: string

  constructor() {
    this.#entries = []

    this.#boundary = '--------------------------'
    for (let i = 0; i < 24; i++) {
      this.#boundary += Math.floor(Math.random() * 10).toString(16)
    }
  }

  append(
    field: string,
    value: ReadableStream | string,
    options?: EntryOptions,
  ): void {
    let valueStream: ReadableStream
    if (isReadableStream(value)) {
      valueStream = value
    } else {
      valueStream = new Blob([value]).stream()
    }

    this.#entries.push({
      field,
      value: valueStream,
      options: options || { type: 'string' },
    })
  }

  #multipartBoundary(): ReadableStream {
    return new Blob(['--', this.#boundary, CRLF]).stream()
  }

  #multipartHeader(name: string, type: 'file' | 'string'): ReadableStream {
    let filenamePart

    if (type === 'file') {
      filenamePart = `; filename="${name}"`
    } else {
      filenamePart = ''
    }

    return new Blob([
      `content-disposition: form-data; name="${name}"${filenamePart}`,
      CRLF,
      CRLF,
    ]).stream()
  }

  #multipartBody(stream: ReadableStream): ReadableStream {
    return chainStreams([stream, new Blob([CRLF]).stream()])
  }

  #multipartFooter(): ReadableStream {
    return new Blob(['--', this.#boundary, '--', CRLF]).stream()
  }

  contentType(): string {
    return `multipart/form-data; boundary=${this.#boundary}`
  }

  stream(): ReadableStream {
    const streams: ReadableStream[] = [this.#multipartBoundary()]

    const valueStreams = []
    for (const { field, value, options } of this.#entries) {
      valueStreams.push(this.#multipartHeader(field, options.type))
      valueStreams.push(this.#multipartBody(value))
      valueStreams.push(this.#multipartBoundary())
    }

    if (valueStreams.length) {
      // Remove last boundary as we want a footer instead
      valueStreams.pop()
    }

    streams.push(...valueStreams)

    streams.push(this.#multipartFooter())

    return chainStreams(streams)
  }
}
