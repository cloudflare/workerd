// https://developers.cloudflare.com/pub-sub/

// PubSubMessage represents an incoming PubSub message.
// The message includes metadata about the broker, the client, and the payload
// itself.
interface PubSubMessage {
    // Message ID
    readonly mid: number;
    // MQTT broker FQDN in the form mqtts://BROKER.NAMESPACE.cloudflarepubsub.com:PORT
    readonly broker: string;
    // The MQTT topic the message was sent on.
    readonly topic: string;
    // The client ID of the client that published this message.
    readonly clientId: string;
    // The unique identifier (JWT ID) used by the client to authenticate, if token
    // auth was used.
    readonly jti?: string;
    // A Unix timestamp (seconds from Jan 1, 1970), set when the Pub/Sub Broker
    // received the message from the client.
    readonly receivedAt: number;
    // An (optional) string with the MIME type of the payload, if set by the
    // client.
    readonly contentType: string;
    // Set to 1 when the payload is a UTF-8 string
    // https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901063
    readonly payloadFormatIndicator: number;
    // Pub/Sub (MQTT) payloads can be UTF-8 strings, or byte arrays.
    // You can use payloadFormatIndicator to inspect this before decoding.
    payload: string | Uint8Array;
}

// JsonWebKey extended by kid parameter
interface JsonWebKeyWithKid extends JsonWebKey {
    // Key Identifier of the JWK
    readonly kid: string;
}
