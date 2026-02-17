/**
 * Binding entrypoint for Cloudflare Stream.
 *
 * Usage:
 * - Binding-level operations:
 *   `await env.STREAM.videos.upload`
 *   `await env.STREAM.videos.createDirectUpload`
 *   `await env.STREAM.videos.*`
 *   `await env.STREAM.watermarks.*`
 * - Per-video operations:
 *   `await env.STREAM.video(uid).downloads.*`
 *   `await env.STREAM.video(uid).captions.*`
 *
 * Example usage:
 * ```ts
 * await env.STREAM.video(uid).downloads.create();
 *
 * const video = env.STREAM.video(uid)
 * const captions = video.captions.list();
 * const videoDetails = video.get()
 * ```
 */
interface StreamBinding {
  /**
   * Returns a handle scoped to a single video for per-video operations.
   * @param uid The unique identifier for the video.
   * @returns A handle for per-video operations.
   */
  video(uid: string): StreamVideoHandle;
  /**
   * Uploads a new video from a File.
   * @param file The video file to upload.
   * @returns The uploaded video details.
   */
  upload(file: File): Promise<StreamVideo>;
  /**
   * Uploads a new video from a provided URL.
   * @param url The URL to upload from.
   * @param params Optional upload parameters.
   * @returns The uploaded video details.
   */
  upload(url: string, params?: StreamUploadUrlParams): Promise<StreamVideo>;
  /**
   * Creates a direct upload that allows video uploads without an API key.
   */
  createDirectUpload(
    params: StreamDirectUploadCreateParams
  ): Promise<StreamDirectUpload>;

  videos: StreamVideos;
  watermarks: StreamWatermarks;
}

/**
 * Handle for operations scoped to a single Stream video.
 */
interface StreamVideoHandle {
  /**
   * The unique identifier for the video.
   */
  uid: string;
  /**
   * Get a full videos details
   * @returns The full video details.
   */
  get(): Promise<StreamVideo>;
  /**
   * Edit details for a single video.
   * @param params The fields to update for the video.
   * @returns The updated video details.
   */
  edit(params: StreamEditVideoParams): Promise<StreamVideo>;
  /**
   * Deletes a video and its copies from Cloudflare Stream.
   * @returns A promise that resolves when deletion completes.
   */
  delete(): Promise<void>;
  /**
   * Creates a signed URL token for a video.
   * @returns The signed token that was created.
   */
  generateToken(): Promise<string>;

  downloads: StreamScopedDownloads;
  captions: StreamScopedCaptions;
}

interface StreamVideo {
  /**
   * The unique identifier for the video.
   */
  uid: string;
  /**
   * A user-defined identifier for the media creator.
   */
  creator: string | null;
  /**
   * The thumbnail URL for the video.
   */
  thumbnail: string;
  /**
   * The thumbnail timestamp percentage.
   */
  thumbnailTimestampPct: number;
  /**
   * Indicates whether the video is ready to stream.
   */
  readyToStream: boolean;
  /**
   * The date and time the video became ready to stream.
   */
  readyToStreamAt: string | null;
  /**
   * Processing status information.
   */
  status: StreamVideoStatus;
  /**
   * A user modifiable key-value store.
   */
  meta: Record<string, string>;
  /**
   * The date and time the video was created.
   */
  created: string;
  /**
   * The date and time the video was last modified.
   */
  modified: string;
  /**
   * The date and time at which the video will be deleted.
   */
  scheduledDeletion: string | null;
  /**
   * The size of the video in bytes.
   */
  size: number;
  /**
   * The preview URL for the video.
   */
  preview?: string;
  /**
   * Origins allowed to display the video.
   */
  allowedOrigins: Array<string>;
  /**
   * Indicates whether signed URLs are required.
   */
  requireSignedURLs: boolean | null;
  /**
   * The date and time the video was uploaded.
   */
  uploaded: string | null;
  /**
   * The date and time when the upload URL expires.
   */
  uploadExpiry: string | null;
  /**
   * The maximum size in bytes for direct uploads.
   */
  maxSizeBytes: number | null;
  /**
   * The maximum duration in seconds for direct uploads.
   */
  maxDurationSeconds: number | null;
  /**
   * The video duration in seconds. -1 indicates unknown.
   */
  duration: number;
  /**
   * Input metadata for the original upload.
   */
  input: StreamVideoInput;
  /**
   * Playback URLs for the video.
   */
  hlsPlaybackUrl: string;
  dashPlaybackUrl: string;
  /**
   * The watermark applied to the video, if any.
   */
  watermark: StreamWatermark | null;
  /**
   * The live input UID associated with the video, if any.
   */
  liveInput?: string | null;
  /**
   * The source video UID if this is a clip.
   */
  clippedFrom: string | null;
  /**
   * Public details associated with the video.
   */
  publicDetails: StreamPublicDetails | null;
}

type StreamVideoStatus = {
  /**
   * The current processing state.
   */
  state: string;
  /**
   * The current processing step.
   */
  step?: string;
  /**
   * The percent complete as a string.
   */
  pctComplete?: string;
  /**
   * An error reason code, if applicable.
   */
  errorReasonCode: string;
  /**
   * An error reason text, if applicable.
   */
  errorReasonText: string;
};

type StreamVideoInput = {
  /**
   * The input width in pixels.
   */
  width: number;
  /**
   * The input height in pixels.
   */
  height: number;
};

type StreamPublicDetails = {
  /**
   * The public title for the video.
   */
  title: string | null;
  /**
   * The public share link.
   */
  share_link: string | null;
  /**
   * The public channel link.
   */
  channel_link: string | null;
  /**
   * The public logo URL.
   */
  logo: string | null;
};

type StreamDirectUpload = {
  /**
   * The URL an unauthenticated upload can use for a single multipart request.
   */
  uploadURL: string;
  /**
   * A Cloudflare-generated unique identifier for a media item.
   */
  uid: string;
  /**
   * The watermark profile applied to the upload.
   */
  watermark: StreamWatermark | null;
  /**
   * The scheduled deletion time, if any.
   */
  scheduledDeletion: string | null;
};

type StreamDirectUploadCreateParams = {
  /**
   * The maximum duration in seconds for a video upload.
   */
  maxDurationSeconds: number;
  /**
   * The date and time after upload when videos will not be accepted.
   */
  expiry?: string;
  /**
   * A user-defined identifier for the media creator.
   */
  creator?: string;
  /**
   * A user modifiable key-value store used to reference other systems of record for
   * managing videos.
   */
  meta?: Record<string, string>;
  /**
   * Lists the origins allowed to display the video.
   */
  allowedOrigins?: Array<string>;
  /**
   * Indicates whether the video can be accessed using the UID. When set to `true`,
   * a signed token must be generated with a signing key to view the video.
   */
  requireSignedURLs?: boolean;
  /**
   * The thumbnail timestamp percentage.
   */
  thumbnailTimestampPct?: number;
  /**
   * The date and time at which the video will be deleted. Include `null` to remove
   * a scheduled deletion.
   */
  scheduledDeletion?: string | null;
  /**
   * The watermark profile to apply.
   */
  watermark?: StreamDirectUploadWatermark;
};

type StreamDirectUploadWatermark = {
  /**
   * The unique identifier for the watermark profile.
   */
  uid: string;
};

type StreamUploadUrlParams = {
  /**
   * Lists the origins allowed to display the video. Enter allowed origin
   * domains in an array and use `*` for wildcard subdomains. Empty arrays allow the
   * video to be viewed on any origin.
   */
  allowedOrigins?: Array<string>;
  /**
   * A user-defined identifier for the media creator.
   */
  creator?: string;
  /**
   * A user modifiable key-value store used to reference other systems of
   * record for managing videos.
   */
  meta?: Record<string, string>;
  /**
   * Indicates whether the video can be a accessed using the UID. When
   * set to `true`, a signed token must be generated with a signing key to view the
   * video.
   */
  requireSignedURLs?: boolean;
  /**
   * Indicates the date and time at which the video will be deleted. Omit
   * the field to indicate no change, or include with a `null` value to remove an
   * existing scheduled deletion. If specified, must be at least 30 days from upload
   * time.
   */
  scheduledDeletion?: string | null;
  /**
   * The timestamp for a thumbnail image calculated as a percentage value
   * of the video's duration. To convert from a second-wise timestamp to a
   * percentage, divide the desired timestamp by the total duration of the video. If
   * this value is not set, the default thumbnail image is taken from 0s of the
   * video.
   */
  thumbnailTimestampPct?: number;
  /**
   * The identifier for the watermark profile
   */
  watermarkId?: string;
};

interface StreamScopedCaptions {
  /**
   * Uploads the caption or subtitle file to the endpoint for a specific BCP47 language.
   * One caption or subtitle file per language is allowed.
   * @param language The BCP 47 language tag for the caption or subtitle.
   * @param file The caption or subtitle file to upload.
   * @returns The created caption entry.
   */
  upload(language: string, file: File): Promise<StreamCaption>;
  /**
   * Generate captions or subtitles for the provided language via AI.
   * @param language The BCP 47 language tag to generate.
   * @returns The generated caption entry.
   */
  generate(language: string): Promise<StreamCaption>;
  /**
   * Lists the captions or subtitles.
   * Use the language parameter to filter by a specific language.
   * @param language The optional BCP 47 language tag to filter by.
   * @returns The list of captions or subtitles.
   */
  list(language?: string): Promise<StreamCaption[]>;
  /**
   * Removes the captions or subtitles from a video.
   * @param language The BCP 47 language tag to remove.
   * @returns A promise that resolves when deletion completes.
   */
  delete(language: string): Promise<void>;
}

interface StreamScopedDownloads {
  /**
   * Creates a download for a video when a video is ready to view. Available
   * types are `default` and `audio`. Defaults to `default` when omitted.
   * @param downloadType The download type to create.
   * @returns The current downloads for the video.
   */
  create(downloadType?: StreamDownloadType): Promise<StreamDownloadGetResponse>;
  /**
   * Lists the downloads created for a video.
   * @returns The current downloads for the video.
   */
  get(): Promise<StreamDownloadGetResponse>;
  /**
   * Delete the downloads for a video. Available types are `default` and `audio`.
   * Defaults to `default` when omitted.
   * @param downloadType The download type to delete.
   * @returns A promise that resolves when deletion completes.
   */
  delete(downloadType?: StreamDownloadType): Promise<void>;
}

interface StreamVideos {
  /**
   * Lists all videos in a users account.
   * @returns The list of videos.
   */
  list(): Promise<StreamVideo[]>;
}

interface StreamWatermarks {
  /**
   * Create a new watermark profile
   * @param file The image file to upload
   * @param params The watermark creation parameters.
   * @returns The created watermark profile.
   */
  create(
    file: File,
    params: StreamWatermarkCreateParams
  ): Promise<StreamWatermark>;
  /**
   * Create a new watermark profile
   * @param url The image url to upload
   * @param params The watermark creation parameters.
   * @returns The created watermark profile.
   */
  create(
    url: string,
    params: StreamWatermarkCreateParams
  ): Promise<StreamWatermark>;
  /**
   * Lists all watermark profiles for an account.
   * @returns The list of watermark profiles.
   */
  list(): Promise<StreamWatermark[]>;
  /**
   * Retrieves details for a single watermark profile.
   * @param watermarkId The watermark profile identifier.
   * @returns The watermark profile details.
   */
  get(watermarkId: string): Promise<StreamWatermark>;
  /**
   * Deletes a watermark profile.
   * @param watermarkId The watermark profile identifier.
   * @returns A promise that resolves when deletion completes.
   */
  delete(watermarkId: string): Promise<void>;
}

type StreamEditVideoParams = {
  /**
   * Lists the origins allowed to display the video. Enter allowed origin
   * domains in an array and use `*` for wildcard subdomains. Empty arrays allow the
   * video to be viewed on any origin.
   */
  allowedOrigins?: Array<string>;
  /**
   * A user-defined identifier for the media creator.
   */
  creator?: string;
  /**
   * The maximum duration in seconds for a video upload. Can be set for a
   * video that is not yet uploaded to limit its duration. Uploads that exceed the
   * specified duration will fail during processing. A value of `-1` means the value
   * is unknown.
   */
  maxDurationSeconds?: number;
  /**
   * A user modifiable key-value store used to reference other systems of
   * record for managing videos.
   */
  meta?: Record<string, string>;
  /**
   * Indicates whether the video can be a accessed using the UID. When
   * set to `true`, a signed token must be generated with a signing key to view the
   * video.
   */
  requireSignedURLs?: boolean;
  /**
   * Indicates the date and time at which the video will be deleted. Omit
   * the field to indicate no change, or include with a `null` value to remove an
   * existing scheduled deletion. If specified, must be at least 30 days from upload
   * time.
   */
  scheduledDeletion?: string | null;
  /**
   * The timestamp for a thumbnail image calculated as a percentage value
   * of the video's duration. To convert from a second-wise timestamp to a
   * percentage, divide the desired timestamp by the total duration of the video. If
   * this value is not set, the default thumbnail image is taken from 0s of the
   * video.
   */
  thumbnailTimestampPct?: number;
};

type StreamCaption = {
  /**
   * Whether the caption was generated via AI.
   */
  generated?: boolean;
  /**
   * The language label displayed in the native language to users.
   */
  label: string;
  /**
   * The language tag in BCP 47 format.
   */
  language: string;
  /**
   * The status of a generated caption.
   */
  status?: 'ready' | 'inprogress' | 'error';
};

type StreamDownloadStatus = 'ready' | 'inprogress' | 'error';

type StreamDownloadType = 'default' | 'audio';

type StreamDownload = {
  /**
   * Indicates the progress as a percentage between 0 and 100.
   */
  percentComplete: number;
  /**
   * The status of a generated download.
   */
  status: StreamDownloadStatus;
  /**
   * The URL to access the generated download.
   */
  url?: string;
};

/**
 * An object with download type keys. Each key is optional and only present if that
 * download type has been created.
 */
type StreamDownloadGetResponse = {
  /**
   * The audio-only download. Only present if this download type has been created.
   */
  audio?: StreamDownload;
  /**
   * The default video download. Only present if this download type has been created.
   */
  default?: StreamDownload;
};

type StreamWatermarkPosition =
  | 'upperRight'
  | 'upperLeft'
  | 'lowerLeft'
  | 'lowerRight'
  | 'center';

type StreamWatermark = {
  /**
   * The unique identifier for a watermark profile.
   */
  uid: string;
  /**
   * The size of the image in bytes.
   */
  size: number;
  /**
   * The height of the image in pixels.
   */
  height: number;
  /**
   * The width of the image in pixels.
   */
  width: number;
  /**
   * The date and a time a watermark profile was created.
   */
  created: string;
  /**
   * The source URL for a downloaded image. If the watermark profile was created via
   * direct upload, this field is null.
   */
  downloadedFrom: string | null;
  /**
   * A short description of the watermark profile.
   */
  name: string;
  /**
   * The translucency of the image. A value of `0.0` makes the image completely
   * transparent, and `1.0` makes the image completely opaque. Note that if the image
   * is already semi-transparent, setting this to `1.0` will not make the image
   * completely opaque.
   */
  opacity: number;
  /**
   * The whitespace between the adjacent edges (determined by position) of the video
   * and the image. `0.0` indicates no padding, and `1.0` indicates a fully padded
   * video width or length, as determined by the algorithm.
   */
  padding: number;
  /**
   * The size of the image relative to the overall size of the video. This parameter
   * will adapt to horizontal and vertical videos automatically. `0.0` indicates no
   * scaling (use the size of the image as-is), and `1.0 `fills the entire video.
   */
  scale: number;
  /**
   * The location of the image. Valid positions are: `upperRight`, `upperLeft`,
   * `lowerLeft`, `lowerRight`, and `center`. Note that `center` ignores the
   * `padding` parameter.
   */
  position: StreamWatermarkPosition;
};

type StreamWatermarkCreateParams = {
  /**
   * A short description of the watermark profile.
   */
  name?: string;
  /**
   * The translucency of the image. A value of `0.0` makes the image completely
   * transparent, and `1.0` makes the image completely opaque. Note that if the
   * image is already semi-transparent, setting this to `1.0` will not make the
   * image completely opaque.
   */
  opacity?: number;
  /**
   * The whitespace between the adjacent edges (determined by position) of the
   * video and the image. `0.0` indicates no padding, and `1.0` indicates a fully
   * padded video width or length, as determined by the algorithm.
   */
  padding?: number;
  /**
   * The size of the image relative to the overall size of the video. This
   * parameter will adapt to horizontal and vertical videos automatically. `0.0`
   * indicates no scaling (use the size of the image as-is), and `1.0 `fills the
   * entire video.
   */
  scale?: number;
  /**
   * The location of the image. Valid positions are: `upperRight`, `upperLeft`,
   * `lowerLeft`, `lowerRight`, and `center`. Note that `center` ignores the
   * `padding` parameter.
   */
  position?: StreamWatermarkPosition;
};

/**
 * Error object for Stream binding operations.
 */
interface StreamError extends Error {
  readonly code: number;
  readonly message: string;
  readonly stack?: string;
}
