/**
 * Media binding for transforming media streams.
 * Provides the entry point for media transformation operations.
 */
interface MediaBinding {
	/**
	 * Creates a media transformer from an input stream.
	 * @param media - The input media bytes
	 * @returns A MediaTransformer instance for applying transformations
	 */
	input(media: ReadableStream<Uint8Array>): MediaTransformer
}

/**
 * Media transformer for applying transformation operations to media content.
 * Handles sizing, fitting, and other input transformation parameters.
 */
interface MediaTransformer {
	/**
	 * Applies transformation options to the media content.
	 * @param transform - Configuration for how the media should be transformed
	 * @returns A generator for producing the transformed media output
	 */
	transform(
		transform?: MediaTransformationInputOptions
	): MediaTransformationGenerator

	/**
	 * Generates the final media output with specified options.
	 * @param output - Configuration for the output format and parameters
	 * @returns The final transformation result containing the transformed media
	 */
	output(
		output?: MediaTransformationOutputOptions
	): MediaTransformationResult
}

/**
 * Generator for producing media transformation results.
 * Configures the output format and parameters for the transformed media.
 */
interface MediaTransformationGenerator {
	/**
	 * Generates the final media output with specified options.
	 * @param output - Configuration for the output format and parameters
	 * @returns The final transformation result containing the transformed media
	 */
	output(
		output?: MediaTransformationOutputOptions
	): MediaTransformationResult
}

/**
 * Result of a media transformation operation.
 * Provides multiple ways to access the transformed media content.
 */
interface MediaTransformationResult {
	/**
	 * Returns the transformed media as a readable stream of bytes.
	 * @returns A promise containing a readable stream with the transformed media
	 */
	media(): Promise<ReadableStream<Uint8Array>>
	/**
	 * Returns the transformed media as an HTTP response object.
	 * @returns The transformed media as a Promise<Response>, ready to store in cache or return to users
	 */
	response(): Promise<Response>
	/**
	 * Returns the MIME type of the transformed media.
	 * @returns A promise containing the content type string (e.g., 'image/jpeg', 'video/mp4')
	 */
	contentType(): Promise<string>
}

/**
 * Configuration options for transforming media input.
 * Controls how the media should be resized and fitted.
 */
type MediaTransformationInputOptions = {
	/** How the media should be resized to fit the specified dimensions */
	fit?: 'contain' | 'cover' | 'scale-down'
	/** Target width in pixels */
	width?: number
	/** Target height in pixels */
	height?: number
}

/**
 * Configuration options for Media Transformations output.
 * Controls the format, timing, and type of the generated output.
 */
type MediaTransformationOutputOptions = {
	/**
	 * Output mode determining the type of media to generate
	 */
	mode?: 'video' | 'spritesheet' | 'frame' | 'audio'
	/** Whether to include audio in the output */
	audio?: boolean
	/**
	 * Starting timestamp for frame extraction or start time for clips. (e.g. '2s').
	 */
	time?: string
	/**
	 * Duration for video clips, audio extraction, and spritesheet generation (e.g. '5s').
	 */
	duration?: string
	/**
	 * Number of frames in the spritesheet.
	 */
	imageCount?: number
	/**
	 * Output format for the generated media.
	 */
	format?: 'jpg' | 'png' | 'm4a'
}

/**
 * Error object for media transformation operations.
 * Extends the standard Error interface with additional media-specific information.
 */
interface MediaError extends Error {
  readonly code: number;
  readonly message: string;
  readonly stack?: string;
}

