/** Main interface for creating meetings in the app configured for your binding. */
interface RealtimeKitBinding {
  /**
   * Creates a meeting and returns its details along with a method for adding
   * participants.
   */
  createMeeting(
    options?: RealtimeKitCreateMeetingOptions
  ): Promise<RealtimeKitMeeting>;
}

/** Settings for creating a meeting. */
interface RealtimeKitCreateMeetingOptions {
  /** Human-readable meeting title. */
  title?: string | null;
  /**
   * Start recording when a participant joins.
   * @default false
   */
  record_on_start?: boolean | null;
  /**
   * Start livestreaming when the meeting starts.
   * @default false
   */
  live_stream_on_start?: boolean | null;
  /** Meeting recording settings, which take precedence over app settings. */
  recording_config?: RealtimeKitRecordingConfig;
  /** Transcription and summarization settings. */
  ai_config?: RealtimeKitAIConfig;
  /**
   * Retain meeting chat for a week.
   * @default false
   */
  persist_chat?: boolean;
  /**
   * Generate a meeting summary; requires transcription.
   * @default false
   */
  summarize_on_end?: boolean;
  /**
   * Generate a transcript when the meeting ends.
   * @default false
   */
  transcribe_on_end?: boolean;
  /**
   * Seconds a session stays active after its last participant leaves.
   * Between 60 and 600.
   * @default 60
   */
  session_keep_alive_time_in_secs?: number;
}

/** Details of the participant to add to a meeting. */
interface RealtimeKitAddParticipantOptions {
  /** Your unique identifier for the participant, such as a user ID or email. */
  custom_participant_id: string;
  /** Name of an existing preset to apply, such as group_call_host. */
  preset_name: string;
  /** Participant display name. */
  name?: string | null;
  /** URL of the participant's profile picture. */
  picture?: string | null;
  /** Validity window for the participant authentication token. */
  token?: RealtimeKitParticipantTokenValidity;
}

/**
 * Controls when a participant's join token can be used.
 * The token is valid from not_before up to, but not including, expires_on.
 * The validity period cannot exceed 100 days and defaults to 100 days from now.
 */
interface RealtimeKitParticipantTokenValidity {
  /** RFC 3339 timestamp with a timezone; defaults to now and may be in the past. */
  not_before?: string;
  /**
   * RFC 3339 timestamp with a timezone; must be in the future when issued.
   * Defaults to not_before plus 100 days.
   */
  expires_on?: string;
}

/** Participant details and the authentication token needed to join the meeting. */
interface RealtimeKitParticipant {
  /** Participant UUID. */
  id: string;
  /** The unique participant identifier you provided when adding the participant. */
  custom_participant_id: string;
  /** Preset applied to the participant. */
  preset_name: string;
  /** Supplied participant display name. */
  name?: string | null;
  /** URL of the participant's profile picture. */
  picture?: string | null;
  /** When the participant was created, as an ISO date-time string. */
  created_at: string;
  /** When the participant was last updated, as an ISO date-time string. */
  updated_at: string;
  /** Sensitive authentication token used to join the meeting. Do not log it. */
  token: string;
  /** Time from which the token is valid, as a UTC date-time string. */
  not_before?: string;
  /** Time at which the token expires, as a UTC date-time string. */
  expires_on?: string;
}

/**
 * Meeting details captured when the meeting was created.
 * Use addParticipant() to add participants to this meeting.
 * Use `using` to release the meeting handle when finished.
 */
interface RealtimeKitMeeting extends Disposable {
  /** Unique identifier of the meeting. */
  readonly id: string;
  /** Human-readable meeting title. */
  title?: string;
  /** When the meeting was created, as an ISO date-time string. */
  readonly created_at: string;
  /** When the meeting was last updated, as an ISO date-time string. */
  readonly updated_at: string;
  /** Whether recording starts when a participant joins. */
  record_on_start?: boolean;
  /** Whether livestreaming starts when the meeting starts. */
  live_stream_on_start?: boolean;
  /** Whether meeting chat persists for a week. */
  persist_chat?: boolean;
  /** Whether a meeting summary is generated using transcripts. */
  summarize_on_end?: boolean;
  /** Whether a transcript is generated when the meeting ends. */
  transcribe_on_end?: boolean;
  /** Recording settings for the meeting. Storage credentials are not included. */
  recording_config?: RealtimeKitRecordingConfigResponse;
  /** Transcription and summarization settings. */
  ai_config?: RealtimeKitAIConfig;
  /** Participants cannot join an INACTIVE meeting. */
  status?: 'ACTIVE' | 'INACTIVE';
  /**
   * Session keep-alive duration, 60 to 600 seconds.
   * @default 60
   */
  session_keep_alive_time_in_secs?: number;
  /**
   * Adds a participant to this meeting and returns their details and join token.
   */
  addParticipant(
    options: RealtimeKitAddParticipantOptions
  ): Promise<RealtimeKitParticipant>;
}

/** Meeting-level recording settings, overriding the app's recording settings. */
interface RealtimeKitRecordingConfig {
  /** Maximum recording duration in seconds, between 60 and 86400. */
  max_seconds?: number;
  /** Prefix added to the recording file name. */
  file_name_prefix?: string;
  /** Recorded video encoding, dimensions, and watermark settings. */
  video_config?: RealtimeKitVideoConfig;
  /** Recorded audio encoding and channel settings. */
  audio_config?: RealtimeKitAudioConfig;
  /** Storage destination and credentials for uploading recordings. */
  storage_config?: RealtimeKitStorageConfig;
  /** Whether recordings are uploaded to RealtimeKit's bucket. */
  realtimekit_bucket_config?: RealtimeKitBucketConfig;
  /** Destination for live-streaming the recording. */
  live_streaming_config?: RealtimeKitLivestreamingConfig;
}

/** Recording settings for a meeting, with storage credentials omitted. */
interface RealtimeKitRecordingConfigResponse extends Omit<
  RealtimeKitRecordingConfig,
  'storage_config'
> {
  /** Destination storage with access keys, secrets, passwords, and keys omitted. */
  storage_config?: RealtimeKitStorageConfigResponse;
}

/** Video settings for meeting recordings. */
interface RealtimeKitVideoConfig {
  /**
   * Video codec.
   * @default 'H264'
   */
  codec?: 'H264' | 'VP8' | 'VP9';
  /**
   * Video width in pixels, between 1 and 1920.
   * @default 1280
   */
  width?: number;
  /**
   * Video height in pixels, between 1 and 1920.
   * @default 720
   */
  height?: number;
  /** Watermark image, dimensions, and placement. */
  watermark?: {
    /** URL of the watermark image. */
    url?: string;
    /** Watermark dimensions in pixels. */
    size?: {
      /** Watermark width; minimum 1 pixel. */
      width?: number;
      /** Watermark height; minimum 1 pixel. */
      height?: number;
    };
    /**
     * Watermark placement.
     * @default 'left top'
     */
    position?: 'left top' | 'right top' | 'left bottom' | 'right bottom';
  };
  /**
   * Export a separate video file.
   * @default true
   */
  export_file?: boolean;
}

/** Audio settings for meeting recordings. */
interface RealtimeKitAudioConfig {
  /**
   * Audio codec. VP8/VP9 video automatically uses Vorbis audio, and the audio
   * codec cannot be changed.
   * @default 'AAC'
   */
  codec?: 'MP3' | 'AAC';
  /**
   * Audio channel layout.
   * @default 'stereo'
   */
  channel?: 'mono' | 'stereo';
  /**
   * Export a separate audio file.
   * @default true
   */
  export_file?: boolean;
}

/** Destination and authentication settings for external recording storage. */
interface RealtimeKitStorageConfigBase {
  /** Destination bucket name. */
  bucket: string;
  /**
   * Storage access key; required and nonempty except for Google Cloud Storage.
   * Not included in returned storage details.
   */
  access_key?: string;
  /** Secret key for the storage provider. Not included in returned details. */
  secret?: string;
  /** Storage region; required and nonempty except for Google Cloud Storage. */
  region?: string;
  /** Recording path relative to the bucket root. */
  path?: string;
  /** Authentication method for SFTP storage. */
  auth_method?: 'KEY' | 'PASSWORD';
  /** Destination SSH username for SFTP. */
  username?: string;
  /**
   * SFTP login password, or SSH private-key passphrase when using KEY.
   * Not included in returned storage details.
   */
  password?: string;
  /** Destination SSH hostname for SFTP. */
  host?: string;
  /** Destination SSH port for SFTP. */
  port?: number;
  /** SSH private key for SFTP key authentication. Not included in returned details. */
  private_key?: string;
}

/** Recording destination in Google Cloud Storage, AWS, Azure, DigitalOcean, or SFTP. */
export type RealtimeKitStorageConfig =
  | (RealtimeKitStorageConfigBase &
      (
        | { type: 'gcs' }
        | {
            type: 'aws' | 'azure' | 'digitalocean';
            access_key: string;
            region: string;
          }
        | ({
            type: 'sftp';
            access_key: string;
            region: string;
            username: string;
            host: string;
            port: number;
          } & (
            | { auth_method: 'KEY'; private_key: string }
            | { auth_method: 'PASSWORD'; password: string }
          ))
      ))
  | null;

/** Recording destination details, without access keys, secrets, passwords, or private keys. */
type RealtimeKitStorageConfigResponse =
  | (Omit<
      RealtimeKitStorageConfigBase,
      'access_key' | 'secret' | 'password' | 'private_key'
    > &
      (
        | { type: 'gcs' }
        | {
            type: 'aws' | 'azure' | 'digitalocean';
            region: string;
          }
        | {
            type: 'sftp';
            region: string;
            auth_method: 'KEY' | 'PASSWORD';
            username: string;
            host: string;
            port: number;
          }
      ))
  | null;

/** Controls uploading recordings to RealtimeKit's managed bucket. */
interface RealtimeKitBucketConfig {
  /**
   * Enable managed-bucket uploads. If false, recording download URLs and
   * their expiry are not generated.
   */
  enabled: boolean;
}

/** Live-streaming destination for a recording. */
interface RealtimeKitLivestreamingConfig {
  /** RTMP destination URL. */
  rtmp_url?: string;
}

/** Transcription and summary customization for a meeting. */
interface RealtimeKitAIConfig {
  /** Transcription options. */
  transcription?: RealtimeKitTranscriptionConfig;
  /** Summary options. */
  summarization?: RealtimeKitSummarizationConfig;
}

/** Meeting transcription settings. */
interface RealtimeKitTranscriptionConfig {
  /** Terms to recognize more accurately during transcription. */
  keywords?: string[];
  /**
   * Language used for transcription. If omitted, post-meeting transcription
   * detects the language automatically.
   */
  language?:
    | 'en-US'
    | 'en-IN'
    | 'de'
    | 'hi'
    | 'sv'
    | 'ru'
    | 'pl'
    | 'el'
    | 'fr'
    | 'nl';
  /**
   * Filter offensive language in transcripts.
   * @default false
   */
  profanity_filter?: boolean;
}

/** Meeting summarization settings. */
interface RealtimeKitSummarizationConfig {
  /**
   * Maximum words in the summary, between 150 and 1000.
   * @default 500
   */
  word_limit?: number;
  /**
   * Summary text format.
   * @default 'markdown'
   */
  text_format?: 'plain_text' | 'markdown';
  /**
   * Summary style.
   * @default 'general'
   */
  summary_type?:
    | 'general'
    | 'team_meeting'
    | 'sales_call'
    | 'client_check_in'
    | 'interview'
    | 'daily_standup'
    | 'one_on_one_meeting'
    | 'lecture'
    | 'code_review';
}
