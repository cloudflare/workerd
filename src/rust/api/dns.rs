use jsg::Lock;
use jsg::Struct;
use jsg::v8;
use jsg::v8::ToLocalValue;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum DnsParserError {
    #[error("Invalid hex string: {0}")]
    InvalidHexString(String),
    #[error("ParseInt error: {0}")]
    ParseIntError(#[from] std::num::ParseIntError),
    #[error("Invalid DNS response: {0}")]
    InvalidDnsResponse(String),
    #[error("unknown dns parser error")]
    Unknown,
}

impl From<DnsParserError> for jsg::Error {
    fn from(val: DnsParserError) -> Self {
        Self::new("Error".to_owned(), val.to_string())
    }
}

/// CAA record representation
#[derive(Debug)]
pub struct CaaRecord {
    pub critical: u8,
    pub field: String,
    pub value: String,
}

impl jsg::Type for CaaRecord {}

impl jsg::Struct for CaaRecord {
    fn wrap<'a, 'b>(&self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        unsafe {
            let mut obj = lock.new_object();
            let critical = self.critical.to_local(lock);
            obj.set(lock, "critical", critical);
            let field = self.field.to_local(lock);
            obj.set(lock, "field", field);
            let value = self.value.to_local(lock);
            obj.set(lock, "value", value);
            obj.into()
        }
    }
}

/// NAPTR record representation
#[derive(Debug)]
pub struct NaptrRecord {
    pub flags: String,
    pub service: String,
    pub regexp: String,
    pub replacement: String,
    pub order: u32,
    pub preference: u32,
}

impl jsg::Type for NaptrRecord {}

impl jsg::Struct for NaptrRecord {
    fn wrap<'a, 'b>(&self, lock: &'a mut Lock) -> v8::Local<'b, v8::Value>
    where
        'b: 'a,
    {
        unsafe {
            let mut obj = lock.new_object();
            let flags = self.flags.to_local(lock);
            obj.set(lock, "flags", flags);
            let service = self.service.to_local(lock);
            obj.set(lock, "service", service);
            let regexp = self.regexp.to_local(lock);
            obj.set(lock, "regexp", regexp);
            let replacement = self.replacement.to_local(lock);
            obj.set(lock, "replacement", replacement);
            let order = self.order.to_local(lock);
            obj.set(lock, "order", order);
            let preference = self.preference.to_local(lock);
            obj.set(lock, "preference", preference);
            obj.into()
        }
    }
}

/// Given a vector of strings, converts each slice to UTF-8 from HEX.
///
/// # Errors
/// `DnsParserError::InvalidHexString`
/// `DnsParserError::ParseIntError`
pub fn decode_hex(input: &[&str]) -> Result<Vec<String>, DnsParserError> {
    let mut v = Vec::with_capacity(input.len());

    for slice in input {
        let num = u16::from_str_radix(slice, 16)?;
        let ch = String::from_utf16(&[num])
            .map_err(|_| DnsParserError::InvalidHexString("Invalid UTF-16 sequence".to_owned()))?;
        v.push(ch);
    }

    Ok(v)
}

/// Replacement values needs to be parsed accordingly.
///
/// It has a similar characteristic to CAA and NAPTR records whereas
/// first character contains the length of the input, and the second character
/// is the starting index of the substring. We need to continue parsing until there
/// are no input left, and later join them using "."
///
/// It is important that the returning value doesn't end with dot (".") character.
///
/// # Errors
/// `DnsParserError::InvalidHexString`
/// `DnsParserError::ParseIntError`
pub fn parse_replacement(input: &[&str]) -> jsg::Result<String, DnsParserError> {
    if input.is_empty() {
        return Ok(String::new());
    }

    let mut output: Vec<String> = vec![];
    let mut length_index = 0;
    let mut offset_index = 1;

    // Iterate through each character to parse different frames.
    // Each frame starts with the length of the remaining frame.
    while length_index < input.len() {
        let length = usize::from_str_radix(input[length_index], 16)?;
        let subset = input[offset_index..length + offset_index].to_vec();
        let decoded = decode_hex(&subset)?.join("");

        // We omit the trailing "." from replacements.
        // Cloudflare DNS returns "_sip._udp.sip2sip.info." whereas Node.js removes trailing dot
        if !decoded.is_empty() {
            output.push(decoded);
        }

        length_index += subset.len() + 1;
        offset_index = length_index + 1;
    }

    Ok(output.join("."))
}

// #[jsg::resource]
pub struct DnsUtil {
    pub js: Option<v8::Global<v8::Value>>,
}

// Generated code
pub struct DnsUtilWrapper {
    pub constructor: v8::Global<v8::FunctionTemplate>,
    // context_constructor: Option<Global<FunctionTemplate>>,
}

impl jsg::ResourceWrapper for DnsUtilWrapper {
    fn get_constructor<'a>(&self, lock: &'a mut Lock) -> v8::Local<'a, v8::FunctionTemplate> {
        self.constructor.as_local(lock)
    }
}

impl DnsUtil {
    /// Parses an unknown RR format returned from Cloudflare DNS.
    /// Specification is available at
    /// `<https://datatracker.ietf.org/doc/html/rfc3597>`
    ///
    /// The format of the record is as follows:
    ///   \# <length-in-bytes> <bytes-in-hex>
    ///   \\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67
    ///       |  |  |  |
    ///       |  |  |  - Starting point of the actual data
    ///       |  |  - Length of the field.
    ///       |  - Number representation of "`is_critical`"
    ///       - Length of the data
    ///
    /// Note: Field can be "issuewild", "issue" or "iodef".
    ///
    /// ```
    /// let record = parse_caa_record("\\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67");
    /// assert_eq!(record.critical, false);
    /// assert_eq!(record.field, "issue")
    /// assert_eq!(record.value, "pki.goog")
    /// ```
    /// # Errors
    /// `DnsParserError::InvalidHexString`
    /// `DnsParserError::ParseIntError`
    pub fn parse_caa_record(&self, record: &str) -> Result<CaaRecord, DnsParserError> {
        // Let's remove "\\#" and the length of data from the beginning of the record
        let data = record.split_ascii_whitespace().collect::<Vec<_>>()[2..].to_vec();
        let critical = data[0].parse::<u8>()?;
        let prefix_length = data[1].parse::<usize>()?;

        let field = decode_hex(&data[2..prefix_length + 2])?.join("");
        let value = decode_hex(&data[(prefix_length + 2)..])?.join("");

        // Field can be "issuewild", "issue" or "iodef"
        if field != "issuewild" && field != "issue" && field != "iodef" {
            return Err(DnsParserError::InvalidDnsResponse(format!(
                "Received unknown field '{field}'"
            )));
        }

        Ok(CaaRecord {
            critical,
            field,
            value,
        })
    }

    /// Parses an unknown RR format returned from Cloudflare DNS.
    /// Specification is available at
    /// `<https://datatracker.ietf.org/doc/html/rfc3597>`
    ///
    /// The format of the record is as follows:
    /// \# 37 15 b3 08 ae 01 73 0a 6d 79 2d 73 65 72 76 69 63 65 06 72 65 67 65 78 70 0b 72 65 70 6c 61 63 65 6d 65 6e 74 00
    ///       |--|  |--|  |  |  |  |--------------------------|  |  |--------------|  |  |--------------------------------|
    ///       |     |     |  |  |  |                             |  |                 |  - Replacement
    ///       |     |     |  |  |  |                             |  |                 - Length of first part of the replacement
    ///       |     |     |  |  |  |                             |  - Regexp
    ///       |     |     |  |  |  |                             - Regexp length
    ///       |     |     |  |  |  - Service
    ///       |     |     |  |  - Length of service
    ///       |     |     |  - Flag
    ///       |     |     - Length of flags
    ///       |     - Preference
    ///       - Order
    ///
    /// ```
    /// let record = parse_naptr_record("\\# 37 15 b3 08 ae 01 73 0a 6d 79 2d 73 65 72 76 69 63 65 06 72 65 67 65 78 70 0b 72 65 70 6c 61 63 65 6d 65 6e 74 00");
    /// assert_eq!(record.flags, "s");
    /// assert_eq!(record.service, "my-service");
    /// assert_eq!(record.regexp, "regexp");
    /// assert_eq!(record.replacement, "replacement");
    /// assert_eq!(record.order, 5555);
    /// assert_eq!(record.preference, 2222);
    /// ```
    ///
    /// # Errors
    /// `DnsParserError::InvalidHexString`
    /// `DnsParserError::ParseIntError`
    pub fn parse_naptr_record(&self, record: &str) -> jsg::Result<NaptrRecord, DnsParserError> {
        let data = record.split_ascii_whitespace().collect::<Vec<_>>()[1..].to_vec();

        let order_str = data[1..3].to_vec();
        let order = u32::from_str_radix(&order_str.join(""), 16)?;
        let preference_str = data[3..5].to_vec();
        let preference = u32::from_str_radix(&preference_str.join(""), 16)?;

        let flag_length = usize::from_str_radix(data[5], 16)?;
        let flag_offset = 6;
        let flags = decode_hex(&data[flag_offset..flag_length + flag_offset])?.join("");

        let service_length = usize::from_str_radix(data[flag_offset + flag_length], 16)?;
        let service_offset = flag_offset + flag_length + 1;
        let service = decode_hex(&data[service_offset..service_length + service_offset])?.join("");

        let regexp_length = usize::from_str_radix(data[service_offset + service_length], 16)?;
        let regexp_offset = service_offset + service_length + 1;
        let regexp = decode_hex(&data[regexp_offset..regexp_length + regexp_offset])?.join("");

        let replacement = parse_replacement(&data[regexp_offset + regexp_length..])?;

        Ok(NaptrRecord {
            flags,
            service,
            regexp,
            replacement,
            order,
            preference,
        })
    }
}

// Generated code.
#[expect(clippy::similar_names)]
impl DnsUtil {
    extern "C" fn parse_caa_record_callback(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
        let mut lock = unsafe { jsg::Lock::from_args(args) };
        let args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
        assert!(args.len() > 0);
        let arg0 = args.get(&mut lock, 0);
        let arg0 = unsafe { jsg::v8::ffi::unwrap_string(lock.get_isolate(), arg0.to_ffi()) };
        let this = args.this(&mut lock);
        let self_ = jsg::unwrap_resource::<Self>(&mut lock, this);
        match self_.parse_caa_record(&arg0) {
            Ok(record) => args.set_return_value(record.wrap(&mut lock)),
            Err(err) => {
                todo!("{err}");
            }
        }
    }

    extern "C" fn parse_naptr_record_callback(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
        let mut lock = unsafe { jsg::Lock::from_args(args) };
        let args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
        assert!(args.len() > 0);
        let arg0 = args.get(&mut lock, 0);
        let arg0 = unsafe { jsg::v8::ffi::unwrap_string(lock.get_isolate(), arg0.to_ffi()) };
        let this = args.this(&mut lock);
        let self_ = jsg::unwrap_resource::<Self>(&mut lock, this);
        match self_.parse_naptr_record(&arg0) {
            Ok(record) => args.set_return_value(record.wrap(&mut lock)),
            Err(err) => {
                todo!("{err}");
            }
        }
    }
}

// Generated code.
impl jsg::Resource for DnsUtil {
    fn members() -> Vec<jsg::Member>
    where
        Self: Sized,
    {
        vec![
            jsg::Member::Method {
                name: "parseCaaRecord",
                callback: Self::parse_caa_record_callback,
            },
            jsg::Member::Method {
                name: "parseNaptrRecord",
                callback: Self::parse_naptr_record_callback,
            },
        ]
    }

    fn class_name() -> &'static str {
        "DnsUtil"
    }

    fn js_instance<'a>(&self, lock: &mut Lock) -> Option<v8::Local<'a, v8::Value>> {
        self.js.as_ref().map(|val| val.to_local(lock))
    }

    fn set_js_instance(&mut self, lock: &mut Lock, instance: v8::Local<v8::Value>) {
        self.js = Some(instance.to_global(lock));
    }
}

impl jsg::Type for DnsUtil {}
