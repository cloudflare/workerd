//! `Sec-WebSocket-Extensions` permessage-deflate parameter handling, ported line-for-line from
//! kj-http (kj/compat/http.c++, `kj::_::findValidExtensionOffers` and friends) so the hyper
//! backend mirrors kj's `MANUAL_COMPRESSION` behavior exactly.
//!
//! Note that nothing here *negotiates* on its own initiative: workerd negotiates
//! `Sec-WebSocket-Extensions` itself and these helpers only reproduce how kj interprets the
//! header values the application supplies (client offers on `openWebSocket`, the manual server
//! config passed to `acceptWebSocket`, and the server's response agreement).

/// Mirrors `kj::CompressionParameters`: the negotiated permessage-deflate configuration, in
/// inbound/outbound (rather than client/server) terms so the same struct works on both sides.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CompressionConfig {
    pub outbound_no_context_takeover: bool,
    pub inbound_no_context_takeover: bool,
    pub outbound_max_window_bits: Option<u8>,
    pub inbound_max_window_bits: Option<u8>,
}

/// Whether a `Sec-WebSocket-Extensions` value is being parsed as a client offer (Request) or
/// as a server agreement (Response); mirrors kj's `isAgreement` flag.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ParseAs {
    Offer,
    Agreement,
}

/// An intermediate parse result; mirrors `kj::_::UnverifiedConfig`.
#[derive(Default)]
struct UnverifiedConfig<'a> {
    client_no_context_takeover: bool,
    server_no_context_takeover: bool,
    /// `Some("")` records a parameter that appeared *without* `=value`, which is valid only for
    /// `client_max_window_bits` in an offer.
    client_max_window_bits: Option<&'a str>,
    server_max_window_bits: Option<&'a str>,
}

/// Mirrors `kj::_::splitParts`: split on `delim`, trimming spaces and tabs from each element.
fn split_parts(input: &str, delim: char) -> Vec<&str> {
    input
        .split(delim)
        .map(|part| part.trim_matches([' ', '\t']))
        .collect()
}

/// Mirrors `kj::_::toKeysAndVals` + `populateUnverifiedConfig`: parse one offer's parameter list
/// (everything after the `permessage-deflate` token). Returns `None` for any invalid or repeated
/// parameter, exactly like kj.
fn populate_unverified_config<'a>(params: &[&'a str]) -> Option<UnverifiedConfig<'a>> {
    if params.len() > 4 {
        // More than 4 keys implies repeats/invalid keys.
        return None;
    }
    let mut config = UnverifiedConfig::default();
    for param in params {
        let (key, value) = match param.find('=') {
            Some(idx) => (
                param[..idx].trim_matches([' ', '\t']),
                Some(param[idx + 1..].trim_matches([' ', '\t'])),
            ),
            None => (*param, None),
        };
        match key {
            "client_no_context_takeover" | "server_no_context_takeover" => {
                let setting = if key.starts_with("client") {
                    &mut config.client_no_context_takeover
                } else {
                    &mut config.server_no_context_takeover
                };
                if *setting || value.is_some() {
                    // Repeated key, or a value where none is allowed.
                    return None;
                }
                *setting = true;
            }
            "client_max_window_bits" | "server_max_window_bits" => {
                let setting = if key.starts_with("client") {
                    &mut config.client_max_window_bits
                } else {
                    &mut config.server_max_window_bits
                };
                if setting.is_some() {
                    return None;
                }
                match value {
                    Some("") => {
                        // `x_max_window_bits=` with nothing after the `=` is invalid.
                        return None;
                    }
                    Some(v) => *setting = Some(v),
                    // Received without `=`; retain that fact (valid only for
                    // client_max_window_bits in an offer).
                    None => *setting = Some(""),
                }
            }
            _ => return None,
        }
    }
    Some(config)
}

/// Mirrors `kj::_::validateCompressionConfig`.
fn validate_compression_config(
    config: &UnverifiedConfig<'_>,
    parse_as: ParseAs,
) -> Option<CompressionConfig> {
    let mut result = CompressionConfig::default();

    if let Some(server_bits) = config.server_max_window_bits {
        if server_bits.is_empty() {
            // `server_max_window_bits` requires a value.
            return None;
        }
        let bits: u8 = server_bits.parse().ok().filter(|b| (8..=15).contains(b))?;
        if parse_as == ParseAs::Agreement {
            result.inbound_max_window_bits = Some(bits);
        } else {
            result.outbound_max_window_bits = Some(bits);
        }
    }

    if let Some(client_bits) = config.client_max_window_bits {
        if client_bits.is_empty() {
            if parse_as == ParseAs::Agreement {
                // `client_max_window_bits` must have a value in a Response.
                return None;
            }
            // Value-less in an offer: best compression (15).
            result.inbound_max_window_bits = Some(15);
        } else {
            let bits: u8 = client_bits.parse().ok().filter(|b| (8..=15).contains(b))?;
            if parse_as == ParseAs::Agreement {
                result.outbound_max_window_bits = Some(bits);
            } else {
                result.inbound_max_window_bits = Some(bits);
            }
        }
    }

    if parse_as == ParseAs::Agreement {
        result.outbound_no_context_takeover = config.client_no_context_takeover;
        result.inbound_no_context_takeover = config.server_no_context_takeover;
    } else {
        result.inbound_no_context_takeover = config.client_no_context_takeover;
        result.outbound_no_context_takeover = config.server_no_context_takeover;
    }
    Some(result)
}

/// Mirrors `kj::_::tryExtractParameters`.
fn try_extract_parameters(offer_parts: &[&str], parse_as: ParseAs) -> Option<CompressionConfig> {
    if offer_parts.len() == 1 {
        // Plain `permessage-deflate`.
        return Some(CompressionConfig::default());
    }
    let config = populate_unverified_config(&offer_parts[1..])?;
    validate_compression_config(&config, parse_as)
}

/// Mirrors `kj::_::findValidExtensionOffers`: parse a client's `Sec-WebSocket-Extensions` header
/// into every valid permessage-deflate offer, mapped in *client* terms.
pub fn find_valid_extension_offers(offers: &str) -> Vec<CompressionConfig> {
    let mut result = Vec::new();
    for offer in split_parts(offers, ',') {
        let parts = split_parts(offer, ';');
        if parts.first() != Some(&"permessage-deflate") {
            continue;
        }
        if let Some(mut validated) = try_extract_parameters(&parts, ParseAs::Offer) {
            // Swap inbound/outbound: try_extract_parameters parsed as the server.
            std::mem::swap(
                &mut validated.inbound_no_context_takeover,
                &mut validated.outbound_no_context_takeover,
            );
            std::mem::swap(
                &mut validated.inbound_max_window_bits,
                &mut validated.outbound_max_window_bits,
            );
            result.push(validated);
        }
    }
    result
}

/// Mirrors `kj::_::tryParseExtensionOffers`: accept the first valid offer, in server terms.
pub fn try_parse_extension_offers(offers: &str) -> Option<CompressionConfig> {
    for offer in split_parts(offers, ',') {
        let parts = split_parts(offer, ';');
        if parts.first() != Some(&"permessage-deflate") {
            continue;
        }
        if let Some(config) = try_extract_parameters(&parts, ParseAs::Offer) {
            return Some(config);
        }
    }
    None
}

/// Mirrors `kj::_::tryParseAllExtensionOffers` (`MANUAL_COMPRESSION` server mode): accept the
/// first client offer compatible with the server's manual config.
pub fn try_parse_all_extension_offers(
    offers: &str,
    manual_config: CompressionConfig,
) -> Option<CompressionConfig> {
    for offer in split_parts(offers, ',') {
        let parts = split_parts(offer, ';');
        if parts.first() != Some(&"permessage-deflate") {
            continue;
        }
        if let Some(config) = try_extract_parameters(&parts, ParseAs::Offer)
            && let Some(final_config) = compare_client_and_server_configs(config, manual_config)
        {
            return Some(final_config);
        }
    }
    None
}

/// Mirrors `kj::_::compareClientAndServerConfigs`.
fn compare_client_and_server_configs(
    request_config: CompressionConfig,
    manual_config: CompressionConfig,
) -> Option<CompressionConfig> {
    let mut accepted = manual_config;

    if !manual_config.inbound_no_context_takeover {
        accepted.inbound_no_context_takeover = false;
    }

    if !manual_config.outbound_no_context_takeover {
        accepted.outbound_no_context_takeover = false;
        if request_config.outbound_no_context_takeover {
            // The client restricted the server's context takeover and the server's manual
            // config does not support it: reject this offer.
            return None;
        }
    }

    // client_max_window_bits
    if let (Some(req_bits), Some(manual_bits)) = (
        request_config.inbound_max_window_bits,
        manual_config.inbound_max_window_bits,
    ) {
        if req_bits < manual_bits {
            accepted.inbound_max_window_bits = Some(req_bits);
        }
    } else {
        accepted.inbound_max_window_bits = None;
    }

    // server_max_window_bits
    if let Some(manual_bits) = manual_config.outbound_max_window_bits {
        if let Some(req_bits) = request_config.outbound_max_window_bits
            && req_bits < manual_bits
        {
            accepted.outbound_max_window_bits = Some(req_bits);
        }
    } else {
        accepted.outbound_max_window_bits = None;
        if request_config.outbound_max_window_bits.is_some() {
            // The client requires `server_max_window_bits`, which the manual config does not
            // support: reject this offer.
            return None;
        }
    }
    Some(accepted)
}

/// Mirrors `kj::_::generateExtensionRequest` for a single offer (all kj call sites that matter
/// here pass exactly one).
pub fn generate_extension_request(offer: CompressionConfig) -> String {
    let mut result = String::from("permessage-deflate");
    if offer.outbound_no_context_takeover {
        result.push_str("; client_no_context_takeover");
    }
    if offer.inbound_no_context_takeover {
        result.push_str("; server_no_context_takeover");
    }
    if let Some(bits) = offer.outbound_max_window_bits {
        result.push_str(&format!("; client_max_window_bits={bits}"));
    }
    if let Some(bits) = offer.inbound_max_window_bits {
        result.push_str(&format!("; server_max_window_bits={bits}"));
    }
    result
}

/// Mirrors `kj::_::generateExtensionResponse`.
pub fn generate_extension_response(parameters: CompressionConfig) -> String {
    let mut result = String::from("permessage-deflate");
    if parameters.inbound_no_context_takeover {
        result.push_str("; client_no_context_takeover");
    }
    if parameters.outbound_no_context_takeover {
        result.push_str("; server_no_context_takeover");
    }
    if let Some(bits) = parameters.inbound_max_window_bits {
        result.push_str(&format!("; client_max_window_bits={bits}"));
    }
    if let Some(bits) = parameters.outbound_max_window_bits {
        result.push_str(&format!("; server_max_window_bits={bits}"));
    }
    result
}

/// Mirrors `kj::_::tryParseExtensionAgreement` (client side, parsing the server's Response).
/// The error strings match kj's exactly.
pub fn try_parse_extension_agreement(
    client_offer: Option<&CompressionConfig>,
    agreed_parameters: &str,
) -> Result<CompressionConfig, String> {
    const FAILURE: &str = "Server failed WebSocket handshake: ";

    let Some(client) = client_offer else {
        return Err(format!(
            "{FAILURE}added Sec-WebSocket-Extensions when client did not offer any."
        ));
    };

    let offers = split_parts(agreed_parameters, ',');
    if offers.len() != 1 {
        return Err(format!(
            "{FAILURE}expected exactly one extension (permessage-deflate) but received more than \
             one."
        ));
    }
    let parts = split_parts(offers[0], ';');
    if parts.first() != Some(&"permessage-deflate") {
        return Err(format!(
            "{FAILURE}response included a Sec-WebSocket-Extensions value that was not \
             permessage-deflate."
        ));
    }

    let Some(mut config) = try_extract_parameters(&parts, ParseAs::Agreement) else {
        return Err(format!(
            "{FAILURE}the Sec-WebSocket-Extensions header in the Response included an invalid \
             value."
        ));
    };

    // The server might have ignored the client's hints about the client's compressor; the
    // client still wants to use its own outbound parameters in that case.
    match (
        config.outbound_max_window_bits,
        client.outbound_max_window_bits,
    ) {
        (None, _) => config.outbound_max_window_bits = client.outbound_max_window_bits,
        (Some(response_bits), Some(client_bits)) if client_bits < response_bits => {
            config.outbound_max_window_bits = Some(client_bits);
        }
        _ => {}
    }
    if !config.outbound_no_context_takeover {
        config.outbound_no_context_takeover = client.outbound_no_context_takeover;
    }
    Ok(config)
}
