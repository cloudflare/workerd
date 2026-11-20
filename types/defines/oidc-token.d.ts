/**
 * An `oidc_token` binding: issues OpenID Connect tokens for the Worker's own
 * identity, to exchange for cloud credentials or send to an API as a bearer
 * token. Each binding has one audience, set in its config.
 */
interface OidcTokenBinding {
  /**
   * Returns a newly signed JWT for the configured audience, valid for 5
   * minutes. Each call gets a new `jti`. Call it when you need a token; don't
   * store the token across requests.
   */
  getToken(): Promise<string>;
}
