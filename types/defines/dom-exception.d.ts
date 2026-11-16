/**
 * The **`DOMException`** interface represents an abnormal event (called an exception) that occurs as a result of calling a method or accessing a property of a web API. This is how error conditions are described in web APIs.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/DOMException)
 */
declare class DOMException extends Error {
  constructor(message?: string, name?: string);
  /**
   * The **`message`** read-only property of the DOMException interface returns a string representing a message or description associated with the given error name.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/DOMException/message)
   */
  readonly message: string;
  /**
   * The **`name`** read-only property of the DOMException interface returns a string that contains one of the strings associated with an error name.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/DOMException/name)
   */
  readonly name: string;
  /**
   * The **`code`** read-only property of the DOMException interface returns one of the legacy error code constants, or 0 if none match.
   * @deprecated
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/DOMException/code)
   */
  readonly code: number;
  static readonly INDEX_SIZE_ERR: number;
  static readonly DOMSTRING_SIZE_ERR: number;
  static readonly HIERARCHY_REQUEST_ERR: number;
  static readonly WRONG_DOCUMENT_ERR: number;
  static readonly INVALID_CHARACTER_ERR: number;
  static readonly NO_DATA_ALLOWED_ERR: number;
  static readonly NO_MODIFICATION_ALLOWED_ERR: number;
  static readonly NOT_FOUND_ERR: number;
  static readonly NOT_SUPPORTED_ERR: number;
  static readonly INUSE_ATTRIBUTE_ERR: number;
  static readonly INVALID_STATE_ERR: number;
  static readonly SYNTAX_ERR: number;
  static readonly INVALID_MODIFICATION_ERR: number;
  static readonly NAMESPACE_ERR: number;
  static readonly INVALID_ACCESS_ERR: number;
  static readonly VALIDATION_ERR: number;
  static readonly TYPE_MISMATCH_ERR: number;
  static readonly SECURITY_ERR: number;
  static readonly NETWORK_ERR: number;
  static readonly ABORT_ERR: number;
  static readonly URL_MISMATCH_ERR: number;
  static readonly QUOTA_EXCEEDED_ERR: number;
  static readonly TIMEOUT_ERR: number;
  static readonly INVALID_NODE_TYPE_ERR: number;
  static readonly DATA_CLONE_ERR: number;
  get stack(): any;
  set stack(value: any);
}
