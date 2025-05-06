/**
 * The body of an R2 Event Notification sent to a Queue
 */
type R2EventNotification = {
  /**
   * The owner of the R2 Bucket.
   */
  account: string
  /**
   * The name of the R2 Bucket.
   */
  bucket: string
  /**
   * The time when the R2 action took place.
   */
  eventTime: Date
} & (
  | {
      /**
       * The R2 action that triggered the Event Notification rule.
       */
      action: 'PutObject'
      object: {
        /**
         * The name of the object that triggered the Event Notification rule.
         */
        key: string
        /**
         * The object size in bytes.
         */
        size: number
        /**
         * The object eTag.
         */
        eTag: string
      }
    }
  | {
      action: 'DeleteObject'
      object: {
        key: string
      }
    }
  | {
      action: 'CompleteMultipartUpload'
      object: {
        key: string
        size: number
        eTag: string
      }
    }
  | {
      action: 'AbortMultipartUpload'
      object: {
        key: string
      }
    }
  | {
      action: 'CopyObject'
      object: {
        key: string
        size: number
        eTag: string
      }
      /**
       * The name of the bucket and object that an R2 object was copied from.
       */
      copySource: {
        bucket: string
        object: string
      }
    }
  | {
      action: 'LifecycleDeletion'
      object: {
        key: string
      }
    }
)
