// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

type Fetcher = {
  fetch: typeof fetch;
};

type RawInfoResponse =
  | { format: "image/svg+xml" }
  | {
      format: string;
      file_size: number;
      width: number;
      height: number;
    };

class TransformationResultImpl implements TransformationResult {
  public constructor(private readonly bindingsResponse: Response) {}

  public contentType(): string {
    const contentType = this.bindingsResponse.headers.get("content-type");
    if (!contentType) {
      throw new ImagesErrorImpl(
        "IMAGES_TRANSFORM_ERROR 9523: No content-type on bindings response",
        9523
      );
    }

    return contentType;
  }

  public image(): ReadableStream<Uint8Array> {
    return this.bindingsResponse.body || new ReadableStream();
  }

  public response(): Response {
    return new Response(this.image(), {
      headers: {
        "content-type": this.contentType(),
      },
    });
  }
}

// Read input into memory for now, as it's difficult to stream multipart
// requests and this is what the backend uses.
//
// The interface takes streams to allow streaming later.
async function streamToBlob(stream: ReadableStream<Uint8Array>): Promise<Blob> {
  return new Response(stream).blob();
}

class ImageTransformerImpl implements ImageTransformer {
  private transforms: Transform[];
  private consumed: boolean;

  public constructor(
    private readonly fetcher: Fetcher,
    private readonly stream: ReadableStream<Uint8Array>
  ) {
    this.transforms = [];
    this.consumed = false;
  }

  public transform(transform: Transform): ImageTransformerImpl {
    this.transforms.push(transform);
    return this;
  }

  public async output(options: OutputOptions): Promise<TransformationResult> {
    if (this.consumed) {
      throw new ImagesErrorImpl(
        "IMAGES_TRANSFORM_ERROR 9525: ImageTransformer consumed; you may only call .output() once",
        9525
      );
    }
    this.consumed = true;

    const body = new FormData();
    body.append("image", await streamToBlob(this.stream));
    body.append("output_format", options.format);
    if (options.quality !== undefined) {
      body.append("output_quality", options.quality.toString());
    }

    if (options.background !== undefined) {
      body.append("background", options.background);
    }

    body.append("transforms", JSON.stringify(this.transforms));

    const response = await this.fetcher.fetch(
      "https://js.images.cloudflare.com/transform",
      {
        method: "POST",
        body,
      }
    );

    await throwErrorIfErrorResponse("TRANSFORM", response);

    return new TransformationResultImpl(response);
  }
}

class ImagesBindingImpl implements ImagesBinding {
  public constructor(private readonly fetcher: Fetcher) {}

  public async info(stream: ReadableStream<Uint8Array>): Promise<InfoResponse> {
    const body = new FormData();
    body.append("image", await streamToBlob(stream));

    const response = await this.fetcher.fetch(
      "https://js.images.cloudflare.com/info",
      {
        method: "POST",
        body,
      }
    );

    await throwErrorIfErrorResponse("INFO", response);

    const r = (await response.json()) as RawInfoResponse;

    if ("file_size" in r) {
      return {
        fileSize: r.file_size,
        width: r.width,
        height: r.height,
        format: r.format,
      };
    }

    return r;
  }

  public input(stream: ReadableStream<Uint8Array>): ImageTransformer {
    return new ImageTransformerImpl(this.fetcher, stream);
  }
}

class ImagesErrorImpl extends Error implements ImagesError {
  public constructor(
    message: string,
    public readonly code: number
  ) {
    super(message);
  }
}

async function throwErrorIfErrorResponse(
  operation: string,
  response: Response
): Promise<void> {
  const statusHeader = response.headers.get("cf-images-binding") || "";

  const match = /err=(\d+)/.exec(statusHeader);

  if (match && match[1]) {
    throw new ImagesErrorImpl(
      `IMAGES_${operation}_${await response.text()}`.trim(),
      Number.parseInt(match[1])
    );
  }

  if (response.status > 399) {
    throw new ImagesErrorImpl(
      `Unexpected error response ${response.status}: ${(
        await response.text()
      ).trim()}`,
      9523
    );
  }
}

export default function makeBinding(env: { fetcher: Fetcher }): ImagesBinding {
  return new ImagesBindingImpl(env.fetcher);
}
