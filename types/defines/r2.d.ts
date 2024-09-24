/**
 * The body of an R2 Event Notification sent to a Queue
 */
interface R2EventNotification {
  /**
   * The owner of the R2 Bucket.
   */
	account: string
  /**
   * The name of the R2 Bucket.
   */
	bucket: string
	object?: {
    /**
     * The name of the object that triggered the Event Notification rule.
     */
	  key: string
    /**
     * The object size in bytes. Not present for deletions.
     */
	  size?: number
    /**
     * The object eTag. Not present for deletions.
     */
	  eTag?: string
	}
  /**
   * The R2 action that triggered the Event Notification rule.
   */
	action: string
  /**
   * The time when the R2 action took place.
   */
	eventTime: Date
	copySource?: {
    /**
     * The name of the bucket and object that an R2 object was copied from.
     * Only present for CopyObject notifications.
     */
	  bucket: string
	  object: string
	}
}
