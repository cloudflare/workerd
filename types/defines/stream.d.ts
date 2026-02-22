/**
 * Binding entrypoint for Cloudflare Stream.
 *
 * Usage:
 * - Binding-level operations:
 *   `await env.STREAM.liveInputs.*`
 *   `await env.STREAM.videos.*`
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
   * The handle exposes `get`, `edit`, `delete`, and nested `downloads`/`captions`.
   * @param uid The unique identifier for the video.
   * @returns A handle for per-video operations.
   */
  video(uid: string): StreamVideoHandle;

  videos: StreamVideos;
  keys: StreamSigningKeys;
  liveInputs: StreamLiveInputs;
  watermarks: StreamWatermarks;
  signedUrlTokens: StreamSignedUrlTokens;
  uploads: StreamUploads;
  directUploads: StreamDirectUploads;
}

/**
 * Handle for operations scoped to a single Stream video.
 * Use this to fetch or update the video, and to manage downloads and captions.
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
  thumbnailTimestampPct: number | null;
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
  playback: StreamVideoPlayback;
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
  errReasonCode: string;
  /**
   * An error reason text, if applicable.
   */
  errReasonText: string;
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

type StreamVideoPlayback = {
  /**
   * The HLS playback URL.
   */
  hls: string;
  /**
   * The DASH playback URL.
   */
  dash: string;
};

type StreamPublicDetails = {
  /**
   * The internal media identifier.
   */
  media_id?: number;
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

interface StreamSigningKeys {
  /**
   * Creates an RSA private key in PEM and JWK formats. Key files are only displayed
   * once after creation. Keys are created, used, and deleted independently of
   * videos, and every key can sign any video.
   * @returns The newly created signing key.
   */
  create(): Promise<StreamSigningKey>;
  /**
   * Deletes signing keys and revokes all signed URLs generated with the key.
   * @param id The signing key identifier.
   * @returns A promise that resolves when deletion completes.
   */
  delete(id: string): Promise<void>;
  /**
   * Lists the video ID and creation date and time when a signing key was created.
   * @returns The list of signing keys.
   */
  list(): Promise<StreamSigningKeySummary[]>;
}

interface StreamLiveInputs {
  /**
   * Creates a live input, and returns credentials that you or your users can use to
   * stream live video to Cloudflare Stream.
   * @param params Optional parameters to configure the live input.
   * @returns The created live input details.
   */
  create(params?: StreamLiveInputCreateParams): Promise<StreamLiveInput>;
  /**
   * Updates a specified live input.
   * @param liveInputId The live input identifier.
   * @param params The fields to update.
   * @returns The updated live input details.
   */
  update(
    liveInputId: string,
    params: StreamLiveInputUpdateParams
  ): Promise<StreamLiveInput>;
  /**
   * Lists the live inputs created for an account. When include_counts is true,
   * returns a response object with counts; otherwise returns an array.
   * @param params Optional parameters to filter or include counts.
   * @returns The list response or array of live inputs.
   */
  list(
    params?: StreamLiveInputListParams
  ): Promise<StreamLiveInputListResponse>;
  /**
   * Prevents a live input from being streamed to and makes the live input
   * inaccessible to any future API calls.
   * @param liveInputId The live input identifier.
   * @returns A promise that resolves when deletion completes.
   */
  delete(liveInputId: string): Promise<void>;
  /**
   * Retrieves details of an existing live input.
   * @param liveInputId The live input identifier.
   * @returns The live input details.
   */
  get(liveInputId: string): Promise<StreamLiveInput>;

  outputs: StreamLiveInputOutputs;
}

interface StreamLiveInputOutputs {
  /**
   * Creates a new output that can be used to simulcast or restream live video to
   * other RTMP or SRT destinations. Outputs are always linked to a specific live
   * input — one live input can have many outputs.
   * @param liveInputId The live input identifier.
   * @param params The output configuration.
   * @returns The created output.
   */
  create(
    liveInputId: string,
    params: StreamLiveInputOutputCreateParams
  ): Promise<StreamLiveInputOutput>;
  /**
   * Updates the state of an output.
   * @param liveInputId The live input identifier.
   * @param outputId The output identifier.
   * @param params The fields to update.
   * @returns The updated output.
   */
  update(
    liveInputId: string,
    outputId: string,
    params: StreamLiveInputOutputUpdateParams
  ): Promise<StreamLiveInputOutput>;
  /**
   * Retrieves all outputs associated with a specified live input.
   * @param liveInputId The live input identifier.
   * @returns The list of outputs.
   */
  list(liveInputId: string): Promise<StreamLiveInputOutput[]>;
  /**
   * Deletes an output and removes it from the associated live input.
   * @param liveInputId The live input identifier.
   * @param outputId The output identifier.
   * @returns A promise that resolves when deletion completes.
   */
  delete(liveInputId: string, outputId: string): Promise<void>;
}

interface StreamWatermarks {
  /**
   * Creates watermark profiles using a single `HTTP POST multipart/form-data`
   * request.
   * @param params The watermark creation parameters.
   * @returns The created watermark profile.
   */
  create(params: StreamWatermarkCreateParams): Promise<StreamWatermark>;
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

interface StreamSignedUrlTokens {
  /**
   * Creates a signed URL token for a video or live input. If params are omitted, defaults are used.
   * @param videoId The video or live input identifier.
   * @param params Optional token creation parameters.
   * @returns The signed URL token response.
   */
  create(
    videoId: string,
    params?: StreamSignedUrlTokenCreateParams
  ): Promise<StreamSignedUrlTokenResponse>;
}

interface StreamUploads {
  /**
   * Uploads a video file using a single multipart request. The file field must be
   * named `file`.
   * @param file The video file to upload.
   * @returns The uploaded video details.
   */
  upload(file: File): Promise<StreamVideo>;
}

interface StreamDirectUploads {
  /**
   * Creates a one-time direct upload URL for unauthenticated uploads.
   * @param params The direct upload creation parameters.
   * @returns The created direct upload details.
   */
  create(params: StreamDirectUploadCreateParams): Promise<StreamDirectUpload>;
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
  /**
   * The date and time when the video upload URL is no longer valid for
   * direct user uploads.
   */
  uploadExpiry?: string;
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
  status?: 'ready' | 'inprogress' | 'completed' | 'error';
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

/**
 * The current connection state for a live input.
 */
type StreamLiveInputStatusState = 'disconnected' | 'connecting' | 'connected';

/**
 * The ingest protocol reported by the live input.
 */
type StreamLiveInputIngestProtocol = 'rtmp' | 'srt' | 'webrtc' | 'rtp' | '';

type StreamLiveInputStatusBase = {
  /**
   * The connection state for this status entry.
   */
  state: StreamLiveInputStatusState;
  /**
   * RFC3339 timestamp of when this state was entered.
   */
  statusEnteredAt: string;
  /**
   * The ingest protocol reported by the live input.
   */
  ingestProtocol: StreamLiveInputIngestProtocol;
  /**
   * Optional reason for the status entry.
   */
  reason?: string;
};

type StreamLiveInputStatusCurrent = StreamLiveInputStatusBase & {
  /**
   * RFC3339 timestamp of the last status refresh.
   */
  statusLastSeen: string;
};

type StreamLiveInputStatusHistory = StreamLiveInputStatusBase;

type StreamLiveInputStatus = {
  /**
   * The current status entry.
   */
  current: StreamLiveInputStatusCurrent;
  /**
   * Historical status entries in descending recency.
   */
  history: Array<StreamLiveInputStatusHistory>;
};

/**
 * Details about a live input.
 */
type StreamLiveInput = {
  /**
   * A unique identifier for a live input.
   */
  uid: string;
  /**
   * Details for streaming to an live input using RTMPS.
   */
  rtmps: StreamLiveInputRtmps;
  /**
   * The date and time the live input was created.
   */
  created: string;
  /**
   * The date and time the live input was last modified.
   */
  modified: string;
  /**
   * A user modifiable key-value store used to reference other systems of record for
   * managing live inputs.
   */
  meta: Record<string, string>;
  /**
   * The connection status of a live input.
   */
  status: StreamLiveInputStatus | null;
  /**
   * Indicates the number of days after which the live inputs recordings will be
   * deleted. When a stream completes and the recording is ready, the value is used
   * to calculate a scheduled deletion date for that recording. Omit the field to
   * indicate no change, or include with a `null` value to remove an existing
   * scheduled deletion.
   */
  deleteRecordingAfterDays: number | null;

  /**
   * Details for playback from an live input using RTMPS.
   */
  rtmpsPlayback?: StreamLiveInputRtmpsPlayback;
  /**
   * Details for streaming to a live input using SRT.
   */
  srt?: StreamLiveInputSrt;
  /**
   * Details for playback from an live input using SRT.
   */
  srtPlayback?: StreamLiveInputSrtPlayback;
  /**
   * Details for streaming to a live input using WebRTC.
   */
  webRTC?: StreamLiveInputWebRtc;
  /**
   * Details for playback from a live input using WebRTC.
   */
  webRTCPlayback?: StreamLiveInputWebRtcPlayback;
  /**
   * Records the input to a Cloudflare Stream video. Behavior depends on the mode. In
   * most cases, the video will initially be viewable as a live video and transition
   * to on-demand after a condition is satisfied.
   */
  recording?: StreamLiveInputRecording;
  /**
   * Indicates the creator associated with this live input.
   */
  defaultCreator?: string;
  /**
   * Custom hostnames configured for this input.
   */
  customHostnames?: Array<StreamLiveInputCustomHostname>;
  /**
   * Indicates if low-latency is preferred.
   */
  preferLowLatency?: boolean;
};

type StreamLiveInputCustomHostname = {
  id: string;
  name: string;
};

type StreamLiveInputRecording = {
  /**
   * Lists the origins allowed to display videos created with this input. Enter
   * allowed origin domains in an array and use `*` for wildcard subdomains. An empty
   * array allows videos to be viewed on any origin.
   */
  allowedOrigins?: Array<string>;
  /**
   * Disables reporting the number of live viewers when this property is set to
   * `true`.
   */
  hideLiveViewerCount?: boolean;
  /**
   * Specifies the recording behavior for the live input. Set this value to `off` to
   * prevent a recording. Set the value to `automatic` to begin a recording and
   * transition to on-demand after Stream Live stops receiving input.
   */
  mode?: 'off' | 'automatic';
  /**
   * Indicates if a video using the live input has the `requireSignedURLs` property
   * set. Also enforces access controls on any video recording of the livestream with
   * the live input.
   */
  requireSignedURLs?: boolean;
  /**
   * Determines the amount of time a live input configured in `automatic` mode should
   * wait before a recording transitions from live to on-demand. `0` is recommended
   * for most use cases and indicates the platform default should be used.
   */
  timeoutSeconds?: number;
};

type StreamLiveInputRtmps = {
  /**
   * The secret key to use when streaming via RTMPS to a live input.
   */
  streamKey: string;
  /**
   * The RTMPS URL you provide to the broadcaster, which they stream live video to.
   */
  url: string;
};

type StreamLiveInputRtmpsPlayback = {
  /**
   * The secret key to use for playback via RTMPS.
   */
  streamKey: string;
  /**
   * The URL used to play live video over RTMPS.
   */
  url: string;
};

type StreamLiveInputSrt = {
  /**
   * The secret key to use when streaming via SRT to a live input.
   */
  passphrase: string;
  /**
   * The identifier of the live input to use when streaming via SRT.
   */
  streamId: string;
  /**
   * The SRT URL you provide to the broadcaster, which they stream live video to.
   */
  url: string;
};

type StreamLiveInputSrtPlayback = {
  /**
   * The secret key to use for playback via SRT.
   */
  passphrase: string;
  /**
   * The identifier of the live input to use for playback via SRT.
   */
  streamId: string;
  /**
   * The URL used to play live video over SRT.
   */
  url: string;
};

type StreamLiveInputWebRtc = {
  /**
   * The WebRTC URL you provide to the broadcaster, which they stream live video to.
   */
  url: string;
};

type StreamLiveInputWebRtcPlayback = {
  /**
   * The URL used to play live video over WebRTC.
   */
  url: string;
};

type StreamLiveInputListResponse =
  | {
      liveInputs: Array<StreamLiveInputListResponseItem>;
      /**
       * The total number of remaining live inputs based on cursor position.
       */
      range?: number;
      /**
       * The total number of live inputs that match the provided filters.
       */
      total?: number;
    }
  | Array<StreamLiveInputListResponseItem>;

type StreamLiveInputListResponseItem = {
  /**
   * The unique identifier for the live input.
   */
  uid: string;
  /**
   * The date and time the live input was created.
   */
  created: string;
  /**
   * The date and time the live input was last modified.
   */
  modified: string;
  /**
   * A user modifiable key-value store for the live input.
   */
  meta: Record<string, string>;
  /**
   * The number of days after which recordings are deleted, if set.
   */
  deleteRecordingAfterDays: number | null;
};

type StreamLiveInputCreateParams = {
  /**
   * Sets the creator ID asssociated with this live input.
   */
  defaultCreator?: string;
  /**
   * Indicates the number of days after which the live inputs recordings will be
   * deleted. When a stream completes and the recording is ready, the value is used
   * to calculate a scheduled deletion date for that recording. Omit the field to
   * indicate no change, or include with a `null` value to remove an existing
   * scheduled deletion.
   */
  deleteRecordingAfterDays?: number;
  /**
   * A user modifiable key-value store used to reference other systems of record for
   * managing live inputs.
   */
  meta?: Record<string, string>;
  /**
   * Records the input to a Cloudflare Stream video. Behavior depends on the mode. In
   * most cases, the video will initially be viewable as a live video and transition
   * to on-demand after a condition is satisfied.
   */
  recording?: StreamLiveInputRecording;
};

type StreamLiveInputUpdateParams = {
  /**
   * Sets the creator ID asssociated with this live input.
   */
  defaultCreator?: string;
  /**
   * Indicates the number of days after which the live inputs recordings will be
   * deleted. When a stream completes and the recording is ready, the value is used
   * to calculate a scheduled deletion date for that recording. Omit the field to
   * indicate no change, or include with a `null` value to remove an existing
   * scheduled deletion.
   */
  deleteRecordingAfterDays?: number;
  /**
   * A user modifiable key-value store used to reference other systems of record for
   * managing live inputs.
   */
  meta?: Record<string, string>;
  /**
   * Records the input to a Cloudflare Stream video. Behavior depends on the mode. In
   * most cases, the video will initially be viewable as a live video and transition
   * to on-demand after a condition is satisfied.
   */
  recording?: StreamLiveInputRecording;
};

type StreamLiveInputListParams = {
  /**
   * Includes the total number of videos associated with the submitted query parameters.
   */
  include_counts?: boolean;
};

type StreamLiveInputOutput = {
  /**
   * A unique identifier for the output.
   */
  uid: string;
  /**
   * The URL an output uses to restream.
   */
  url: string;
  /**
   * When enabled, live video streamed to the associated live input will be sent to
   * the output URL. When disabled, live video will not be sent to the output URL,
   * even when streaming to the associated live input. Use this to control precisely
   * when you start and stop simulcasting to specific destinations like YouTube and
   * Twitch.
   */
  enabled: boolean;
  /**
   * The current status for the output.
   */
  status: StreamLiveInputStatus | null;
  /**
   * The streamKey used to authenticate against an output's target.
   */
  streamKey?: string;
};

type StreamLiveInputOutputCreateParams = {
  /**
   * The streamKey used to authenticate against an output's target.
   */
  streamKey: string;
  /**
   * The URL an output uses to restream.
   */
  url: string;
  /**
   * When enabled, live video streamed to the associated live input will be sent to
   * the output URL. When disabled, live video will not be sent to the output URL,
   * even when streaming to the associated live input. Use this to control precisely
   * when you start and stop simulcasting to specific destinations like YouTube and
   * Twitch.
   */
  enabled?: boolean;
};

type StreamLiveInputOutputUpdateParams = {
  /**
   * When enabled, live video streamed to the associated live input will be sent to
   * the output URL. When disabled, live video will not be sent to the output URL,
   * even when streaming to the associated live input. Use this to control precisely
   * when you start and stop simulcasting to specific destinations like YouTube and
   * Twitch.
   */
  enabled: boolean;
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

type StreamWatermarkCreateParams =
  | {
      /**
       * The image file to upload.
       */
      file: File;
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
    }
  | {
      /**
       * The image URL to copy.
       */
      url: string;
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

type StreamSignedUrlTokenResponse = {
  /**
   * The signed token used with the signed URLs feature.
   */
  token: string;
};

type StreamSignedUrlTokenCreateParams = {
  /**
   * The optional ID of a Stream signing key. If present, the `pem` field
   * is also required.
   */
  id?: string;
  /**
   * The optional list of access rule constraints on the token. Access
   * can be blocked or allowed based on an IP, IP range, or by country. Access rules
   * are evaluated from first to last. If a rule matches, the associated action is
   * applied and no further rules are evaluated.
   */
  accessRules?: Array<StreamSignedUrlTokenAccessRule>;
  /**
   * The optional boolean value that enables using signed tokens to
   * access MP4 download links for a video.
   */
  downloadable?: boolean;
  /**
   * The optional unix epoch timestamp that specifies the time after a
   * token is not accepted. The maximum time specification is 24 hours from issuing
   * time. If this field is not set, the default is one hour after issuing.
   */
  exp?: number;
  /**
   * The optional unix epoch timestamp that specifies the time before a
   * token is not accepted. If this field is not set, the default is one hour
   * before issuing.
   */
  nbf?: number;
  /**
   * The optional base64 encoded private key in PEM format associated
   * with a Stream signing key. If present, the `id` field is also required.
   */
  pem?: string;
  /**
   * Additional token flags.
   */
  flags?: StreamSignedUrlTokenFlags;
};

type StreamSignedUrlTokenFlags = {
  /**
   * When true, indicates the token is for original (non-encoded) content.
   */
  original?: boolean;
};

type StreamSignedUrlTokenAccessRule = {
  /**
   * The action to take when a request matches a rule. If the action is `block`, the
   * signed token blocks views for viewers matching the rule.
   */
  action?: 'allow' | 'block';
  /**
   * An array of 2-letter country codes in ISO 3166-1 Alpha-2 format used to match
   * requests.
   */
  country?: Array<string>;
  /**
   * An array of IPv4 or IPV6 addresses or CIDRs used to match requests.
   */
  ip?: Array<string>;
  /**
   * Lists available rule types to match for requests. An `any` type matches all
   * requests and can be used as a wildcard to apply default actions after other
   * rules.
   */
  type?: 'any' | 'ip.src' | 'ip.geoip.country';
};

type StreamSigningKey = {
  /**
   * Identifier.
   */
  id: string;
  /**
   * The date and time a signing key was created.
   */
  created: string;
  /**
   * The signing key in JWK format.
   */
  jwk: string;
  /**
   * The signing key in PEM format.
   */
  pem: string;
};

type StreamSigningKeySummary = {
  /**
   * Identifier.
   */
  id: string;
  /**
   * Identifier in snake case.
   */
  key_id: string;
  /**
   * The date and time a signing key was created.
   */
  created: string;
};

/**
 * Error object for Stream binding operations.
 */
interface StreamError extends Error {
  readonly code: number;
  readonly message: string;
  readonly stack?: string;
}
