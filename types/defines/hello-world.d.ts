/**
 * Hello World binding to serve as an explanatory example. DO NOT USE
 */
interface HelloWorldBinding {
  /**
   * Retrieve the current stored value
   */
  get(): Promise<{ value: string; ms?: number }>;
  /**
   * Set a new stored value
   */
  set(value: string): Promise<void>;
}
