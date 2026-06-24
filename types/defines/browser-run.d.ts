type BrowserRunLifecycleEvent =
  | 'load'
  | 'domcontentloaded'
  | 'networkidle0'
  | 'networkidle2';

type BrowserRunResourceType =
  | 'document'
  | 'stylesheet'
  | 'image'
  | 'media'
  | 'font'
  | 'script'
  | 'texttrack'
  | 'xhr'
  | 'fetch'
  | 'prefetch'
  | 'eventsource'
  | 'websocket'
  | 'manifest'
  | 'signedexchange'
  | 'ping'
  | 'cspviolationreport'
  | 'preflight'
  | 'other';

/** Options fields shared by all quick actions. */
interface BrowserRunBaseOptions {
  /** Adds `<script>` tags into the page with the desired URL or content.
   * @see https://pptr.dev/api/puppeteer.frameaddscripttagoptions
   */
  addScriptTag?: Array<{
    content?: string;
    url?: string;
    type?: string;
    id?: string;
  }>;
  /** Adds `<link rel="stylesheet">` or `<style>` tags into the page.
   * @see https://pptr.dev/api/puppeteer.frameaddstyletagoptions
   */
  addStyleTag?: Array<{ content?: string; url?: string }>;
  /** Provide credentials for HTTP authentication. @see https://pptr.dev/api/puppeteer.credentials */
  authenticate?: { username: string; password: string };
  /** Set cookies before navigating. @see https://pptr.dev/api/puppeteer.cookieparam */
  cookies?: Array<{
    name: string;
    value: string;
    url?: string;
    domain?: string;
    path?: string;
    secure?: boolean;
    httpOnly?: boolean;
    sameSite?: 'Strict' | 'Lax' | 'None';
    expires?: number;
    priority?: 'Low' | 'Medium' | 'High';
    sameParty?: boolean;
    sourceScheme?: 'Unset' | 'NonSecure' | 'Secure';
    sourcePort?: number;
    partitionKey?: string;
  }>;
  /** Emulate a specific CSS media type (e.g. `"screen"`, `"print"`). */
  emulateMediaType?: string;
  /** Navigation options. @see https://pptr.dev/api/puppeteer.gotooptions */
  gotoOptions?: {
    /** Navigation timeout in milliseconds (max 60 000). @default 30000 */
    timeout?: number;
    /** When to consider navigation complete. @default "domcontentloaded" */
    waitUntil?: BrowserRunLifecycleEvent | BrowserRunLifecycleEvent[];
    referer?: string;
    referrerPolicy?: string;
  };
  /** Block requests matching these regex patterns. Mutually exclusive with `allowRequestPattern`. */
  rejectRequestPattern?: string[];
  /** Only allow requests matching these regex patterns. Mutually exclusive with `rejectRequestPattern`. */
  allowRequestPattern?: string[];
  /** Block requests of these resource types. Mutually exclusive with `allowResourceTypes`. */
  rejectResourceTypes?: BrowserRunResourceType[];
  /** Only allow requests of these resource types. Mutually exclusive with `rejectResourceTypes`. */
  allowResourceTypes?: BrowserRunResourceType[];
  /** Additional HTTP headers sent with every request. */
  setExtraHTTPHeaders?: Record<string, string>;
  /** Whether JavaScript is enabled on the page. */
  setJavaScriptEnabled?: boolean;
  /** Override the default user agent string.
   * @default "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36"
   * */
  userAgent?: string;
  /** Set the browser viewport size.
   * @see https://pptr.dev/api/puppeteer.viewport
   * @default {width:1920,height:1080}
   * */
  viewport?: {
    width: number;
    height: number;
    deviceScaleFactor?: number;
    isMobile?: boolean;
    isLandscape?: boolean;
    hasTouch?: boolean;
  };
  /** Wait for a CSS selector to appear in the page before proceeding.
   * @see https://pptr.dev/api/puppeteer.waitforselectoroptions
   */
  waitForSelector?: {
    selector: string;
    hidden?: true;
    visible?: true;
    /** Timeout in milliseconds. Max 120000 */
    timeout?: number;
  };
  /** Wait for a fixed delay in milliseconds before proceeding. Max 120000 */
  waitForTimeout?: number;
  /** When true, continue on best-effort when awaited events fail or timeout. */
  bestAttempt?: boolean;
  /** Maximum duration in milliseconds for the browser action after page load. Max 120000 */
  actionTimeout?: number;
  /** Cache time to live in seconds (0-86400). Set to 0 to disable.
   * @default 5
   */
  cacheTTL?: number;
}

/** Common options shared by all quick actions. Exactly one of `url` or `html` must be provided.*/
type BrowserRunCommonOptions =
  | (BrowserRunBaseOptions & {
      /** URL to navigate to, e.g. `"https://example.com"`. */
      url: string;
    })
  | (BrowserRunBaseOptions & {
      /** Set the HTML content of the page directly. */
      html: string;
    });

type BrowserRunPuppeteerScreenshotOptions = {
  /** @default "png" */
  type?: 'png' | 'jpeg' | 'webp';
  /** @default "binary" */
  encoding?: 'binary' | 'base64';
  quality?: number;
  fullPage?: boolean;
  clip?: {
    x: number;
    y: number;
    width: number;
    height: number;
    scale?: number;
  };
  omitBackground?: boolean;
  optimizeForSpeed?: boolean;
  captureBeyondViewport?: boolean;
  fromSurface?: boolean;
};

type BrowserRunScreenshotOptions = BrowserRunCommonOptions & {
  /** CSS selector of the element to screenshot. */
  selector?: string;
  /** When true, scroll the entire page before taking the screenshot. */
  scrollPage?: boolean;
  /** @see https://pptr.dev/api/puppeteer.screenshotoptions */
  screenshotOptions?: BrowserRunPuppeteerScreenshotOptions;
};

type BrowserRunPDFOptions = BrowserRunCommonOptions & {
  /** @see https://pptr.dev/api/puppeteer.pdfoptions */
  pdfOptions?: {
    /** @default 1 */
    scale?: number;
    /** @default false */
    displayHeaderFooter?: boolean;
    headerTemplate?: string;
    footerTemplate?: string;
    /** @default false */
    printBackground?: boolean;
    /** @default false */
    landscape?: boolean;
    pageRanges?: string;
    /** @default "letter" */
    format?:
      | 'letter'
      | 'legal'
      | 'tabloid'
      | 'ledger'
      | 'a0'
      | 'a1'
      | 'a2'
      | 'a3'
      | 'a4'
      | 'a5'
      | 'a6';
    width?: string | number;
    height?: string | number;
    /** @default false */
    preferCSSPageSize?: boolean;
    margin?: {
      top?: string | number;
      right?: string | number;
      bottom?: string | number;
      left?: string | number;
    };
    /** @default false */
    omitBackground?: boolean;
    /** @default true */
    tagged?: boolean;
    /** @default false */
    outline?: boolean;
    /** @default 30000 */
    timeout?: number;
  };
};

type BrowserRunScrapeOptions = BrowserRunCommonOptions & {
  /** CSS selectors to scrape. At least one element is required. */
  elements: Array<{ selector: string }>;
};

type BrowserRunLinksOptions = BrowserRunCommonOptions & {
  /** When true, only return links that are visible on the page. @default false */
  visibleLinksOnly?: boolean;
  /** When true, exclude links pointing to external domains. @default false */
  excludeExternalLinks?: boolean;
};

type BrowserRunSnapshotOptions = BrowserRunCommonOptions & {
  /** @see https://pptr.dev/api/puppeteer.screenshotoptions */
  screenshotOptions?: Omit<BrowserRunPuppeteerScreenshotOptions, 'encoding'>;
};

interface BrowserRunJsonBaseOptions {
  /** Custom AI models to try in order. Max 3. Falls back to next on error. */
  custom_ai?: Array<{
    /** Model ID in `<provider>/<model_name>` format, e.g. `"workers-ai/@cf/meta/llama-3.3-70b-instruct-fp8-fast"`. */
    model: string;
    /** Bearer token. Not needed for workers-ai models. */
    authorization?: string;
  }>;
}

/**
 * Options for the `json` quick action.
 * At least one of `prompt` or `response_format` must be provided.
 */
type BrowserRunJsonOptions = BrowserRunCommonOptions &
  BrowserRunJsonBaseOptions &
  (
    | {
        /** Natural-language prompt describing what data to extract. */
        prompt: string;
        /** Structured output schema for the AI model. @see https://developers.cloudflare.com/workers-ai/json-mode/ */
        response_format?: AiTextGenerationResponseFormat;
      }
    | {
        /** Natural-language prompt describing what data to extract. */
        prompt?: string;
        /** Structured output schema for the AI model. @see https://developers.cloudflare.com/workers-ai/json-mode/ */
        response_format: AiTextGenerationResponseFormat;
      }
  );

type BrowserRunContentOptions = BrowserRunCommonOptions;
type BrowserRunMarkdownOptions = BrowserRunCommonOptions;

type BrowserRunResponseMeta = {
  /** HTTP status code of the rendered page */
  status: number;
  /** Page title */
  title: string;
};

/** Success response for `content` action. */
type BrowserRunContentSuccessResponse = {
  success: true;
  /** Extracted HTML content */
  result: string;
  meta: BrowserRunResponseMeta;
};

/** Success response for `links` action. */
type BrowserRunLinksSuccessResponse = {
  success: true;
  /** Extracted links */
  result: string[];
};

/** Success response for `scrape` action. */
type BrowserRunScrapeSuccessResponse = {
  success: true;
  result: Array<{
    /** The CSS selector used to find elements. */
    selector: string;
    /** Array of elements matching the selector. */
    results: Array<{
      /** Outer HTML of the element. */
      html: string;
      /** Text content of the element. */
      text: string;
      /** Width of the element in pixels. */
      width: number;
      /** Height of the element in pixels. */
      height: number;
      /** Top position of the element relative to the viewport in pixels. */
      top: number;
      /** Left position of the element relative to the viewport in pixels. */
      left: number;
      /** Array of HTML attributes on the element. */
      attributes: Array<{
        /** Attribute name. */
        name: string;
        /** Attribute value. */
        value: string;
      }>;
    }>;
  }>;
};

/** Success response for `snapshot` action. */
type BrowserRunSnapshotSuccessResponse = {
  success: true;
  result: {
    /** HTML content of the page. */
    content: string;
    /** Base64-encoded screenshot image. */
    screenshot: string;
  };
  meta: BrowserRunResponseMeta;
};

/** Success response for `json` action. */
type BrowserRunJsonSuccessResponse = {
  success: true;
  /** JSON data extracted from the page using an AI model */
  result: Record<string, unknown>;
};

/** Success response for `markdown` action. */
type BrowserRunMarkdownSuccessResponse = {
  success: true;
  /** Extracted markdown content */
  result: string;
};

/** Error response for BrowserRun actions. */
type BrowserRunErrorResponse = {
  success: false;
  errors: { message: string; code?: number; detail?: string; path?: string }[];
};

/** Error response for BrowserRun `json` action. */
type BrowserRunJsonErrorResponse = BrowserRunErrorResponse & {
  /** Raw AI response text for debugging */
  rawAiResponse?: string;
};

/**
 * Browser Run API binding for automating headless browsers.
 * @see https://developers.cloudflare.com/browser-run/
 */
declare abstract class BrowserRun {
  /**
   * Send a raw HTTP request to the Browser Run API.
   * Used by libraries like `@cloudflare/puppeteer` to acquire and connect to a browser instance.
   * @see https://developers.cloudflare.com/browser-run/
   */
  fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response>;

  /**
   * Take a screenshot of a web page.
   * @param action - Must be `'screenshot'`.
   * @param options - Screenshot options including viewport, selectors, and image format.
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - Binary image data with `Content-Type: image/png`, `image/jpeg`, or `image/webp` (when `encoding: 'binary'`, the default)
   * - Data URI string with `Content-Type: text/plain` (when `encoding: 'base64'`)
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(
    action: 'screenshot',
    options: BrowserRunScreenshotOptions
  ): Promise<Response>;

  /**
   * Generate a PDF of a web page.
   * @param action - Must be `'pdf'`.
   * @param options - PDF generation options including page size, margins, and headers/footers.
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - Binary PDF data with `Content-Type: application/pdf`
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(action: 'pdf', options: BrowserRunPDFOptions): Promise<Response>;

  /**
   * Get the HTML content of a web page.
   * @param action - Must be `'content'`.
   * @param options - Navigation and page interaction options.
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - `BrowserRunContentSuccessResponse` JSON with `Content-Type: application/json`
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(
    action: 'content',
    options: BrowserRunContentOptions
  ): Promise<Response>;

  /**
   * Scrape elements from a web page by CSS selector.
   * @param action - Must be `'scrape'`.
   * @param options - Scrape options with CSS selectors for elements to extract.
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - `BrowserRunScrapeSuccessResponse` JSON with `Content-Type: application/json`
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(
    action: 'scrape',
    options: BrowserRunScrapeOptions
  ): Promise<Response>;

  /**
   * Extract all links from a web page.
   * @param action - Must be `'links'`.
   * @param options - Options to filter visible or internal links only.
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - `BrowserRunLinksSuccessResponse` JSON with `Content-Type: application/json`
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(
    action: 'links',
    options: BrowserRunLinksOptions
  ): Promise<Response>;

  /**
   * Get both the HTML content and a base64-encoded screenshot of a web page.
   * @param action - Must be `'snapshot'`.
   * @param options - Snapshot options including screenshot settings (encoding is always base64).
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - `BrowserRunSnapshotSuccessResponse` JSON with `Content-Type: application/json`
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(
    action: 'snapshot',
    options: BrowserRunSnapshotOptions
  ): Promise<Response>;

  /**
   * Extract structured JSON data from a web page using AI.
   * @param action - Must be `'json'`.
   * @param options - JSON extraction options with prompt or response_format schema.
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - `BrowserRunJsonSuccessResponse` JSON with `Content-Type: application/json`
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   * - HTTP 422 with code `2012` for HTML-to-markdown conversion failures
   * - HTTP 422/500 for AI extraction failures (may include `rawAiResponse` field)
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(
    action: 'json',
    options: BrowserRunJsonOptions
  ): Promise<Response>;

  /**
   * Convert a web page to Markdown.
   * @param action - Must be `'markdown'`.
   * @param options - Navigation and page interaction options.
   * @returns A `Response` containing one of:
   *
   * **Success (HTTP 200):**
   * - `BrowserRunMarkdownSuccessResponse` JSON with `Content-Type: application/json`
   *
   * **Error:**
   * - `BrowserRunErrorResponse` JSON with appropriate HTTP status code (400, 422, 429, 500, 503)
   * - HTTP 422 with code `2012` for HTML-to-markdown conversion failures
   *
   * **Headers:**
   * - `X-Browser-Ms-Used`: Browser time consumed in milliseconds (set when status < 500)
   */
  quickAction(
    action: 'markdown',
    options: BrowserRunMarkdownOptions
  ): Promise<Response>;
}
