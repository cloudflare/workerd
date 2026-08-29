interface Hyperdrive {
  /**
   * Connect directly to Hyperdrive as if it's your database, returning a TCP socket.
   *
   * Calling this method returns an identical socket to if you call
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
   * A synthetic IPv4 address (in the reserved 240.0.0.0/4 range) that, like the
   * host field, is only valid within the context of the currently running
   * Worker and, when passed into the `connect()` function from the
   * "cloudflare:sockets" module, will connect to the Hyperdrive instance for
   * your database. This is provided for database drivers that require the host
   * to be an IP literal rather than a hostname.
   */
  readonly ip: string;
  /*
   * The port that must be paired the the host field when connecting.
   */
  readonly port: number;
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

/**
 * A handle to a dynamically-provisioned Hyperdrive connection, returned by
 * `HyperdriveApi.get()`.
 */
interface HyperdriveDynamic extends Disposable {
  /**
   * The database name to use when connecting through this Hyperdrive.
   */
  readonly database: Promise<string>;
  /*
   * The randomly generated user to use when authenticating to your
   * database via Hyperdrive.
   */
  readonly user: Promise<string>;
  /*
   * The randomly generated password to use when authenticating to your
   * database via Hyperdrive.
   */
  readonly password: Promise<string>;
  /**
   * Open a TCP socket to the target database through this Hyperdrive.
   *
   */
  connect(): Promise<Socket>;
}

/**
 * Binding that provisions Hyperdrive connections at request time, rather than
 * from static configuration.
 */
interface HyperdriveDynamicApi {
  /**
   * Provision a connection for the database described by `args`.
   *
   */
  get(args: HyperdriveDynamicConfig): Promise<HyperdriveDynamic>;
  /**
   * Get a pre-generated connection string used for connecting to dynamic Hyperdrive.
   */
  getHyperdriveConnectionString(connectionString: string): Promise<string>;
}

/**
 * Parameters identifying the database that a dynamically-provisioned
 * Hyperdrive connection should target.
 */
interface HyperdriveDynamicConfig {
  /**
   * Generated connection string to pass into the dynamic worker.
   *
   */
  dynamicHyperdriveConnectionString: string;
  /**
   * Connection string for the origin database Hyperdrive should connect to.
   * Contains credentials, so treat it as a secret.
   *
   * The scheme selects the database engine. PostgreSQL origins are supported.
   */
  connectionString: string;
  /**
   * Region in which to place the connection pool. See the Hyperdrive
   * documentation for the set of supported regions.
   */
  targetRegion: string;
  /**
   * Whether Hyperdrive should cache query results for this connection.
   */
  cachingEnabled?: boolean;
  /**
   * Maximum number of connections the pool may open to the origin database.
   * Defaults to 60.
   */
  maxConnections?: number;
}
