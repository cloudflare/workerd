/**
 * The interface for "version_metadata" binding
 * providing metadata about the Worker Version using this binding.
 */
export type WorkerVersionMetadata = {
  /** The ID of the Worker Version using this binding */
  id: string;
  /** The tag of the Worker Version using this binding */
  tag: string;
  /** The timestamp of when the Worker Version was uploaded */
  timestamp: string;
}
