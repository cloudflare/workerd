interface Hyperdrive {
  /**
   * Connect directly to Hyperdrive as if it's your database, returning a TCP socket.
   *
   * Calling this method returns an idential socket to if you call
   * `connect("host:port")` using the `host` and `port` fields from this object.
   * Pick whichever approach works better with your preferred DB client library.
   *
   * Note that this socket is not yet authenticated -- it's expected that your
   * code (or preferably, the client library of your choice) will authenticate
   * using the information in this class's readonly fields.
   */
  connect(): Socket;

  /**
   * A valid DB connection string that can be passed straight into the typical
   * client library/driver/ORM. This will typically be the easiest way to use
   * Hyperdrive.
   */
  readonly connectionString: string;

  /*
   * A randomly generated hostname that is only valid within the context of the
   * currently running Worker which, when passed into `connect()` function from
   * the "cloudflare:sockets" module, will connect to the Hyperdrive instance
   * for your database.
   */
  readonly host: string;
  /*
   * The port that must be paired the the host field when connecting.
   */
  readonly port: string;
  /*
   * The username to use when authenticating to your database via Hyperdrive.
   * Unlike the host and password, this will be the same every time 
   */
  readonly user: string;
  /*
   * The randomly generated password to use when authenticating to your
   * database via Hyperdrive. Like the host field, this password is only valid
   * within the context of the currently running Worker instance from which
   * it's read.
   */
  readonly password: string;
  /*
   * The name of the database to connect to.
   */
  readonly database: string;
}
