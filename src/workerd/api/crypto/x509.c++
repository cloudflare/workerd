#include "impl.h"
#include "x509.h"
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

KJ_DECLARE_NON_POLYMORPHIC(STACK_OF(ASN1_OBJECT));

namespace workerd::api {

namespace {

static constexpr int kX509NameFlagsMultiline = ASN1_STRFLGS_ESC_2253 | ASN1_STRFLGS_ESC_CTRL |
    ASN1_STRFLGS_UTF8_CONVERT | XN_FLAG_SEP_MULTILINE | XN_FLAG_FN_SN;

static constexpr int kX509NameFlagsRFC2253WithinUtf8JSON =
    XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB & ~ASN1_STRFLGS_ESC_CTRL;

kj::Maybe<kj::Own<BIO>> newBio() {
  auto ptr = BIO_new(BIO_s_mem());
  if (ptr == nullptr) return kj::none;
  return kj::disposeWith<BIO_free_all>(ptr);
}

kj::Maybe<kj::Own<BIO>> loadBio(kj::ArrayPtr<const kj::byte> raw) {
  static constexpr int32_t kMaxSize = kj::maxValue;
  if (raw.size() > kMaxSize) return kj::none;
  KJ_IF_SOME(bio, newBio()) {
    int written = BIO_write(bio.get(), raw.begin(), raw.size());
    if (written != raw.size()) return kj::none;
    return kj::mv(bio);
  }
  return kj::none;
}

int NoPasswordCallback(char* buf, int size, int rwflag, void* u) {
  return 0;
}

kj::String toString(BIO* bio) {
  BUF_MEM* mem;
  BIO_get_mem_ptr(bio, &mem);
  auto result = kj::heapArray<char>(mem->data, mem->length + 1);
  result[result.size() - 1] = '\0';  // NUL-terminate.
  return kj::String(kj::mv(result));
}

bool isSafeAltName(const char* name, size_t length, bool utf8) {
  for (size_t i = 0; i < length; i++) {
    char c = name[i];
    switch (c) {
      case '"':
      case '\\':
        // These mess with encoding rules.
        // Fall through.
      case ',':
        // Commas make it impossible to split the list of subject alternative
        // names unambiguously, which is why we have to escape.
        // Fall through.
      case '\'':
        // Single quotes are unlikely to appear in any legitimate values, but they
        // could be used to make a value look like it was escaped (i.e., enclosed
        // in single/double quotes).
        return false;
      default:
        if (utf8) {
          // In UTF8 strings, we require escaping for any ASCII control character,
          // but NOT for non-ASCII characters. Note that all bytes of any code
          // point that consists of more than a single byte have their MSB set.
          if (static_cast<unsigned char>(c) < ' ' || c == '\x7f') {
            return false;
          }
        } else {
          // Check if the char is a control character or non-ASCII character. Note
          // that char may or may not be a signed type. Regardless, non-ASCII
          // values will always be outside of this range.
          if (c < ' ' || c > '~') {
            return false;
          }
        }
    }
  }
  return true;
}

void printAltName(BIO* out, const char* name, size_t length, bool utf8, const char* safe_prefix) {
  if (isSafeAltName(name, length, utf8)) {
    // For backward-compatibility, append "safe" names without any
    // modifications.
    if (safe_prefix != nullptr) {
      BIO_printf(out, "%s:", safe_prefix);
    }
    BIO_write(out, name, length);
  } else {
    // If a name is not "safe", we cannot embed it without special
    // encoding. This does not usually happen, but we don't want to hide
    // it from the user either. We use JSON compatible escaping here.
    BIO_write(out, "\"", 1);
    if (safe_prefix != nullptr) {
      BIO_printf(out, "%s:", safe_prefix);
    }
    for (size_t j = 0; j < length; j++) {
      char c = static_cast<char>(name[j]);
      if (c == '\\') {
        BIO_write(out, "\\\\", 2);
      } else if (c == '"') {
        BIO_write(out, "\\\"", 2);
      } else if ((c >= ' ' && c != ',' && c <= '~') || (utf8 && (c & 0x80))) {
        // Note that the above condition explicitly excludes commas, which means
        // that those are encoded as Unicode escape sequences in the "else"
        // block. That is not strictly necessary, and Node.js itself would parse
        // it correctly either way. We only do this to account for third-party
        // code that might be splitting the string at commas (as Node.js itself
        // used to do).
        BIO_write(out, &c, 1);
      } else {
        // Control character or non-ASCII character. We treat everything as
        // Latin-1, which corresponds to the first 255 Unicode code points.
        const char hex[] = "0123456789abcdef";
        char u[] = {'\\', 'u', '0', '0', hex[(c & 0xf0) >> 4], hex[c & 0x0f]};
        BIO_write(out, u, sizeof(u));
      }
    }
    BIO_write(out, "\"", 1);
  }
}

void printLatin1AltName(BIO* out, const ASN1_IA5STRING* name, const char* safe_prefix = nullptr) {
  printAltName(out, reinterpret_cast<const char*>(name->data), name->length, false, safe_prefix);
}

void printUtf8AltName(BIO* out, const ASN1_UTF8STRING* name, const char* safe_prefix = nullptr) {
  printAltName(out, reinterpret_cast<const char*>(name->data), name->length, true, safe_prefix);
}

bool printGeneralName(BIO* out, const GENERAL_NAME* gen) {
  if (gen->type == GEN_DNS) {
    ASN1_IA5STRING* name = gen->d.dNSName;
    BIO_write(out, "DNS:", 4);
    // Note that the preferred name syntax (see RFCs 5280 and 1034) with
    // wildcards is a subset of what we consider "safe", so spec-compliant DNS
    // names will never need to be escaped.
    printLatin1AltName(out, name);
  } else if (gen->type == GEN_EMAIL) {
    ASN1_IA5STRING* name = gen->d.rfc822Name;
    BIO_write(out, "email:", 6);
    printLatin1AltName(out, name);
  } else if (gen->type == GEN_URI) {
    ASN1_IA5STRING* name = gen->d.uniformResourceIdentifier;
    BIO_write(out, "URI:", 4);
    // The set of "safe" names was designed to include just about any URI,
    // with a few exceptions, most notably URIs that contains commas (see
    // RFC 2396). In other words, most legitimate URIs will not require
    // escaping.
    printLatin1AltName(out, name);
  } else if (gen->type == GEN_DIRNAME) {
    // Earlier versions of Node.js used X509_NAME_oneline to print the X509_NAME
    // object. The format was non standard and should be avoided. The use of
    // X509_NAME_oneline is discouraged by OpenSSL but was required for backward
    // compatibility. Conveniently, X509_NAME_oneline produced ASCII and the
    // output was unlikely to contains commas or other characters that would
    // require escaping. However, it SHOULD NOT produce ASCII output since an
    // RFC5280 AttributeValue may be a UTF8String.
    // Newer versions of Node.js have since switched to X509_NAME_print_ex to
    // produce a better format at the cost of backward compatibility. The new
    // format may contain Unicode characters and it is likely to contain commas,
    // which require escaping. Fortunately, the recently safeguarded function
    // PrintAltName handles all of that safely.
    BIO_printf(out, "DirName:");
    auto tmp = KJ_ASSERT_NONNULL(newBio());
    if (X509_NAME_print_ex(tmp.get(), gen->d.dirn, 0, kX509NameFlagsRFC2253WithinUtf8JSON) < 0) {
      return false;
    }
    char* oline = nullptr;
    long n_bytes = BIO_get_mem_data(tmp.get(), &oline);  // NOLINT(runtime/int)
    KJ_REQUIRE(n_bytes >= 0);
    if (n_bytes > 0) {
      KJ_REQUIRE(oline != nullptr);
    }

    printAltName(out, oline, static_cast<size_t>(n_bytes), true, nullptr);
  } else if (gen->type == GEN_IPADD) {
    BIO_printf(out, "IP Address:");
    const ASN1_OCTET_STRING* ip = gen->d.ip;
    const unsigned char* b = ip->data;
    if (ip->length == 4) {
      BIO_printf(out, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    } else if (ip->length == 16) {
      for (unsigned int j = 0; j < 8; j++) {
        uint16_t pair = (b[2 * j] << 8) | b[2 * j + 1];
        BIO_printf(out, (j == 0) ? "%X" : ":%X", pair);
      }
    } else {
      BIO_printf(out, "<invalid>");
    }
  } else if (gen->type == GEN_RID) {
    // Unlike OpenSSL's default implementation, never print the OID as text and
    // instead always print its numeric representation.
    char oline[256] = {0};
    OBJ_obj2txt(oline, sizeof(oline), gen->d.rid, true);
    BIO_printf(out, "Registered ID:%s", oline);
  } else if (gen->type == GEN_OTHERNAME) {
    // The format that is used here is based on OpenSSL's implementation of
    // GENERAL_NAME_print (as of OpenSSL 3.0.1). Earlier versions of Node.js
    // instead produced the same format as i2v_GENERAL_NAME, which was somewhat
    // awkward, especially when passed to translatePeerCertificate.
    bool unicode = true;
    const char* prefix = nullptr;
    // OpenSSL 1.1.1 does not support othername in GENERAL_NAME_print and may
    // not define these NIDs.
#if OPENSSL_VERSION_MAJOR >= 3
    int nid = OBJ_obj2nid(gen->d.otherName->type_id);
    switch (nid) {
      case NID_id_on_SmtpUTF8Mailbox:
        prefix = "SmtpUTF8Mailbox";
        break;
      case NID_XmppAddr:
        prefix = "XmppAddr";
        break;
      case NID_SRVName:
        prefix = "SRVName";
        unicode = false;
        break;
      case NID_ms_upn:
        prefix = "UPN";
        break;
      case NID_NAIRealm:
        prefix = "NAIRealm";
        break;
    }
#endif  // OPENSSL_VERSION_MAJOR >= 3
    int val_type = gen->d.otherName->value->type;
    if (prefix == nullptr || (unicode && val_type != V_ASN1_UTF8STRING) ||
        (!unicode && val_type != V_ASN1_IA5STRING)) {
      BIO_printf(out, "othername:<unsupported>");
    } else {
      BIO_printf(out, "othername:");
      if (unicode) {
        printUtf8AltName(out, gen->d.otherName->value->value.utf8string, prefix);
      } else {
        printLatin1AltName(out, gen->d.otherName->value->value.ia5string, prefix);
      }
    }
  } else if (gen->type == GEN_X400) {
    // TODO(tniessen): this is what OpenSSL does, implement properly instead
    BIO_printf(out, "X400Name:<unsupported>");
  } else if (gen->type == GEN_EDIPARTY) {
    // TODO(tniessen): this is what OpenSSL does, implement properly instead
    BIO_printf(out, "EdiPartyName:<unsupported>");
  } else {
    // This is safe because X509V3_EXT_d2i would have returned nullptr in this
    // case already.
    KJ_UNREACHABLE;
  }

  return true;
}

bool safeX509SubjectAltNamePrint(BIO* out, X509_EXTENSION* ext) {
  KJ_REQUIRE(OBJ_obj2nid(X509_EXTENSION_get_object(ext)) == NID_subject_alt_name);

  GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(X509V3_EXT_d2i(ext));
  if (names == nullptr) return false;

  bool ok = true;

  for (int i = 0; i < sk_GENERAL_NAME_num(names); i++) {
    GENERAL_NAME* gen = sk_GENERAL_NAME_value(names, i);

    if (i != 0) BIO_write(out, ", ", 2);

    if (!(ok = printGeneralName(out, gen))) {
      break;
    }
  }
  sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);

  return ok;
}

bool safeX509InfoAccessPrint(BIO* out, X509_EXTENSION* ext) {
  KJ_REQUIRE(OBJ_obj2nid(X509_EXTENSION_get_object(ext)) == NID_info_access);

  AUTHORITY_INFO_ACCESS* descs = static_cast<AUTHORITY_INFO_ACCESS*>(X509V3_EXT_d2i(ext));
  if (descs == nullptr) return false;

  bool ok = true;

  for (int i = 0; i < sk_ACCESS_DESCRIPTION_num(descs); i++) {
    ACCESS_DESCRIPTION* desc = sk_ACCESS_DESCRIPTION_value(descs, i);

    if (i != 0) BIO_write(out, "\n", 1);

    char objtmp[80] = {0};
    i2t_ASN1_OBJECT(objtmp, sizeof(objtmp), desc->method);
    BIO_printf(out, "%s - ", objtmp);
    if (!(ok = printGeneralName(out, desc->location))) {
      break;
    }
  }
  sk_ACCESS_DESCRIPTION_pop_free(descs, ACCESS_DESCRIPTION_free);

#if OPENSSL_VERSION_MAJOR < 3
  BIO_write(out, "\n", 1);
#endif

  return ok;
}

void addFingerprintDigest(
    const unsigned char* md, unsigned int md_size, char fingerprint[3 * EVP_MAX_MD_SIZE]) {
  unsigned int i;
  const char hex[] = "0123456789ABCDEF";

  for (i = 0; i < md_size; i++) {
    fingerprint[3 * i] = hex[(md[i] & 0xf0) >> 4];
    fingerprint[(3 * i) + 1] = hex[(md[i] & 0x0f)];
    fingerprint[(3 * i) + 2] = ':';
  }
  fingerprint[(3 * (md_size - 1)) + 2] = '\0';
}

kj::Maybe<kj::String> getFingerprintDigest(const EVP_MD* method, X509* cert) {
  unsigned char md[EVP_MAX_MD_SIZE]{};
  unsigned int md_size;
  auto fingerprint = kj::heapArray<char>(EVP_MD_size(method) * 3);
  if (X509_digest(cert, method, md, &md_size)) {
    addFingerprintDigest(md, md_size, fingerprint.begin());
    return kj::String(kj::mv(fingerprint));
  }
  return kj::none;
}

int optionsToFlags(jsg::Optional<X509Certificate::CheckOptions>& options) {
  X509Certificate::CheckOptions opts = kj::mv(options).orDefault({});
  int flags = 0;
  if (!opts.wildcards.orDefault(true)) {
    flags |= X509_CHECK_FLAG_NO_WILDCARDS;
  }
  if (!opts.partialWildcards.orDefault(true)) {
    flags |= X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS;
  }
  if (opts.multiLabelWildcards.orDefault(false)) {
    flags |= X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS;
  }
  if (opts.singleLabelSubdomains.orDefault(false)) {
    flags |= X509_CHECK_FLAG_SINGLE_LABEL_SUBDOMAINS;
  }
  KJ_IF_SOME(subject, opts.subject) {
    if (subject == "default"_kj) {
      // nothing to do
    } else if (subject == "always"_kj) {
      flags |= X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT;
    } else if (subject == "never"_kj) {
      flags |= X509_CHECK_FLAG_NEVER_CHECK_SUBJECT;
    } else {
      JSG_FAIL_REQUIRE(Error, "Invalid subject option");
    }
  }
  return flags;
}

kj::Maybe<kj::Own<EVP_PKEY>> getInnerPublicKey(X509* cert) {
  EVP_PKEY* pkey = X509_get_pubkey(cert);
  if (pkey == nullptr) {
    return kj::none;
  }
  return kj::disposeWith<EVP_PKEY_free>(pkey);
}

kj::String getModulusString(BIO* bio, const BIGNUM* n) {
  BIO_reset(bio);
  BN_print(bio, n);
  return toString(bio);
}
kj::String getExponentString(BIO* bio, const BIGNUM* e) {
  BIO_reset(bio);
  uint64_t exponent_word = static_cast<uint64_t>(BN_get_word(e));
  BIO_printf(bio, "0x%" PRIx64, exponent_word);
  return toString(bio);
}

kj::Array<kj::byte> getRsaPubKey(RSA* rsa) {
  int size = i2d_RSA_PUBKEY(rsa, nullptr);
  KJ_ASSERT(size >= 0);

  auto buf = kj::heapArray<kj::byte>(size);
  auto data = buf.begin();
  KJ_ASSERT(i2d_RSA_PUBKEY(rsa, &data) >= 0);

  return kj::mv(buf);
}

kj::Maybe<int32_t> getECGroupBits(const EC_GROUP* group) {
  if (group == nullptr) return kj::none;

  int32_t bits = EC_GROUP_order_bits(group);
  if (bits <= 0) return kj::none;

  return bits;
}

kj::Maybe<kj::Array<kj::byte>> eCPointToBuffer(
    const EC_GROUP* group, const EC_POINT* point, point_conversion_form_t form) {
  size_t len = EC_POINT_point2oct(group, point, form, nullptr, 0, nullptr);
  if (len == 0) {
    return kj::none;
  }

  auto buffer = kj::heapArray<kj::byte>(len);

  len = EC_POINT_point2oct(group, point, form, buffer.begin(), buffer.size(), nullptr);
  if (len == 0) {
    return kj::none;
  }

  return kj::mv(buffer);
}

template <const char* (*nid2string)(int nid)>
kj::Maybe<kj::String> getCurveName(const int nid) {
  const char* name = nid2string(nid);
  if (name == nullptr) {
    return kj::none;
  }
  return kj::str(name);
}

kj::Maybe<kj::Array<kj::byte>> getECPubKey(const EC_GROUP* group, EC_KEY* ec) {
  const EC_POINT* pubkey = EC_KEY_get0_public_key(ec);
  if (pubkey == nullptr) return kj::none;

  return eCPointToBuffer(group, pubkey, EC_KEY_get_conv_form(ec));
}

template <X509_NAME* get_name(const X509*)>
kj::Maybe<jsg::JsObject> getX509NameObject(jsg::Lock& js, X509* cert) {
  auto obj = js.obj();
  X509_NAME* name = get_name(cert);
  KJ_ASSERT(name != nullptr);

  int cnt = X509_NAME_entry_count(name);
  KJ_ASSERT(cnt >= 0);

  for (int i = 0; i < cnt; i++) {
    X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, i);
    KJ_ASSERT(entry != nullptr);

    // We intentionally ignore the value of X509_NAME_ENTRY_set because the
    // representation as an object does not allow grouping entries into sets
    // anyway, and multi-value RDNs are rare, i.e., the vast majority of
    // Relative Distinguished Names contains a single type-value pair only.
    const ASN1_OBJECT* type = X509_NAME_ENTRY_get_object(entry);
    ASN1_STRING* value = X509_NAME_ENTRY_get_data(entry);

    // If OpenSSL knows the type, use the short name of the type as the key, and
    // the numeric representation of the type's OID otherwise.
    int type_nid = OBJ_obj2nid(type);
    char type_buf[80] = {0};
    const char* type_str;
    if (type_nid != NID_undef) {
      type_str = OBJ_nid2sn(type_nid);
      KJ_ASSERT(type_str != nullptr);
    } else {
      OBJ_obj2txt(type_buf, sizeof(type_buf), type, true);
      type_str = type_buf;
    }

    auto name = js.str(kj::StringPtr(type_str));

    // The previous implementation used X509_NAME_print_ex, which escapes some
    // characters in the value. The old implementation did not decode/unescape
    // values correctly though, leading to ambiguous and incorrect
    // representations. The new implementation only converts to Unicode and does
    // not escape anything.
    unsigned char* value_str;
    int value_str_size = ASN1_STRING_to_UTF8(&value_str, value);
    if (value_str_size < 0) return kj::none;
    auto v8_value = js.str(kj::StringPtr(reinterpret_cast<char*>(value_str), value_str_size));
    OPENSSL_free(value_str);

    // For backward compatibility, we only create arrays if multiple values
    // exist for the same key. That is not great but there is not much we can
    // change here without breaking things. Note that this creates nested data
    // structures, yet still does not allow representing Distinguished Names
    // accurately.
    if (obj.has(js, name)) {
      auto existing = obj.get(js, name);
      KJ_IF_SOME(a, existing.tryCast<jsg::JsArray>()) {
        a.add(js, v8_value);
      } else {
        obj.set(js, name, js.arr(existing, v8_value));
      }
    } else {
      obj.set(js, name, v8_value);
    }
  }

  return obj;
}

struct StackOfXASN1Disposer: public kj::Disposer {
  void disposeImpl(void* p) const override {
    auto ptr = static_cast<STACK_OF(ASN1_OBJECT)*>(p);
    sk_ASN1_OBJECT_pop_free(ptr, ASN1_OBJECT_free);
  }
};
constexpr StackOfXASN1Disposer stackOfXASN1Disposer;
}  // namespace

kj::Maybe<jsg::Ref<X509Certificate>> X509Certificate::parse(kj::Array<const kj::byte> raw) {
  ClearErrorOnReturn ClearErrorOnReturn;
  KJ_IF_SOME(bio, loadBio(raw)) {
    auto ptr = PEM_read_bio_X509_AUX(bio.get(), nullptr, NoPasswordCallback, nullptr);
    if (ptr == nullptr) {
      MarkPopErrorOnReturn mark_here;
      auto data = raw.begin();
      ptr = d2i_X509(nullptr, &data, raw.size());
      if (ptr == nullptr) {
        throwOpensslError(__FILE__, __LINE__, "X509Certificate::parse()");
      }
    }
    return jsg::alloc<X509Certificate>(ptr);
  }
  return kj::none;
}

kj::Maybe<kj::String> X509Certificate::getSubject() {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(bio, newBio()) {
    if (X509_NAME_print_ex(
            bio.get(), X509_get_subject_name(cert_.get()), 0, kX509NameFlagsMultiline) > 0) {
      return toString(bio.get());
    }
  }
  return kj::none;
}

kj::Maybe<kj::String> X509Certificate::getSubjectAltName() {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(bio, newBio()) {
    int index = X509_get_ext_by_NID(cert_.get(), NID_subject_alt_name, -1);
    if (index < 0) return kj::none;

    X509_EXTENSION* ext = X509_get_ext(cert_.get(), index);
    KJ_ASSERT(ext != nullptr);

    if (!safeX509SubjectAltNamePrint(bio, ext)) {
      return kj::none;
    }

    return toString(bio.get());
  }
  return kj::none;
}

kj::Maybe<kj::String> X509Certificate::getInfoAccess() {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(bio, newBio()) {
    int index = X509_get_ext_by_NID(cert_.get(), NID_info_access, -1);
    if (index < 0) return kj::none;

    X509_EXTENSION* ext = X509_get_ext(cert_.get(), index);
    KJ_REQUIRE(ext != nullptr);

    if (!safeX509InfoAccessPrint(bio, ext)) {
      return kj::none;
    }

    return toString(bio.get());
  }
  return kj::none;
}

kj::Maybe<kj::String> X509Certificate::getIssuer() {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(bio, newBio()) {
    if (X509_NAME_print_ex(
            bio.get(), X509_get_issuer_name(cert_.get()), 0, kX509NameFlagsMultiline) > 0) {
      return toString(bio.get());
    }
  }
  return kj::none;
}

kj::Maybe<jsg::Ref<X509Certificate>> X509Certificate::getIssuerCert() {
  ClearErrorOnReturn clearErrorOnReturn;
  return issuerCert_.map([](jsg::Ref<X509Certificate>& cert) mutable -> jsg::Ref<X509Certificate> {
    return cert.addRef();
  });
}

kj::Maybe<kj::String> X509Certificate::getValidFrom() {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(bio, newBio()) {
    ASN1_TIME_print(bio.get(), X509_get0_notBefore(cert_.get()));
    return toString(bio.get());
  }
  return kj::none;
}

kj::Maybe<kj::String> X509Certificate::getValidTo() {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(bio, newBio()) {
    ASN1_TIME_print(bio.get(), X509_get0_notAfter(cert_.get()));
    return toString(bio.get());
  }
  return kj::none;
}

kj::Maybe<kj::Array<kj::String>> X509Certificate::getKeyUsage() {
  ClearErrorOnReturn clearErrorOnReturn;
  auto ptr = static_cast<STACK_OF(ASN1_OBJECT)*>(
      X509_get_ext_d2i(cert_.get(), NID_ext_key_usage, nullptr, nullptr));
  if (ptr == nullptr) return kj::none;
  auto eku = kj::Own<STACK_OF(ASN1_OBJECT)>(ptr, stackOfXASN1Disposer);
  const int count = sk_ASN1_OBJECT_num(eku.get());
  kj::Vector<kj::String> ext_key_usage(count);
  char buf[256]{};

  int j = 0;
  for (int i = 0; i < count; i++) {
    if (OBJ_obj2txt(buf, sizeof(buf), sk_ASN1_OBJECT_value(eku.get(), i), 1) >= 0) {
      ext_key_usage[j++] = kj::str(buf);
    }
  }

  return ext_key_usage.releaseAsArray();
}

kj::Maybe<kj::Array<const char>> X509Certificate::getSerialNumber() {
  ClearErrorOnReturn clearErrorOnReturn;
  if (ASN1_INTEGER* serial_number = X509_get_serialNumber(cert_.get())) {
    BIGNUM* bn = ASN1_INTEGER_to_BN(serial_number, nullptr);
    if (bn != nullptr) {
      KJ_DEFER(BN_clear_free(bn));
      char* data = BN_bn2hex(bn);
      return kj::arrayPtr<const char>(data, strlen(data))
          .attach(kj::defer([data, len = strlen(data)] { OPENSSL_clear_free(data, len); }));
    }
  }

  return kj::none;
}

kj::Array<kj::byte> X509Certificate::getRaw() {
  ClearErrorOnReturn clearErrorOnReturn;
  int size = i2d_X509(cert_.get(), nullptr);
  auto buf = kj::heapArray<kj::byte>(size);
  auto data = buf.begin();
  KJ_REQUIRE(i2d_X509(cert_.get(), &data) >= 0);
  return kj::mv(buf);
}

kj::Maybe<jsg::Ref<CryptoKey>> X509Certificate::getPublicKey() {
  ClearErrorOnReturn clear_error_on_return;
  auto ptr = X509_get_pubkey(cert_.get());
  if (ptr == nullptr) return kj::none;
  auto pkey = kj::disposeWith<EVP_PKEY_free>(ptr);
  return jsg::alloc<CryptoKey>(CryptoKey::Impl::from(kj::mv(pkey)));
}

kj::Maybe<kj::String> X509Certificate::getPem() {
  ClearErrorOnReturn clearErrorOnReturn;
  KJ_IF_SOME(bio, newBio()) {
    if (PEM_write_bio_X509(bio.get(), cert_.get())) {
      return toString(bio.get());
    }
  }
  return kj::none;
}

kj::Maybe<kj::String> X509Certificate::getFingerprint() {
  ClearErrorOnReturn clearErrorOnReturn;
  return getFingerprintDigest(EVP_sha1(), cert_.get());
}

kj::Maybe<kj::String> X509Certificate::getFingerprint256() {
  ClearErrorOnReturn clearErrorOnReturn;
  return getFingerprintDigest(EVP_sha256(), cert_.get());
}

kj::Maybe<kj::String> X509Certificate::getFingerprint512() {
  ClearErrorOnReturn clearErrorOnReturn;
  return getFingerprintDigest(EVP_sha512(), cert_.get());
}

bool X509Certificate::getIsCA() {
  ClearErrorOnReturn clearErrorOnReturn;
  return X509_check_ca(cert_.get()) == 1;
}

kj::Maybe<kj::String> X509Certificate::checkHost(
    kj::String name, jsg::Optional<CheckOptions> options) {
  ClearErrorOnReturn clearErrorOnReturn;
  char* peername = nullptr;
  switch (
      X509_check_host(cert_.get(), name.begin(), name.size(), optionsToFlags(options), &peername)) {
    case 1: {  // Match!
      if (peername != nullptr) {
        KJ_DEFER(OPENSSL_free(peername));
        return kj::str(peername);
      }
      return kj::mv(name);
    }
    case 0:             // No Match!
      return kj::none;  // No return value is set
    case -2:            // Error!
      JSG_FAIL_REQUIRE(Error, "Invalid name");
    default:  // Error!
      JSG_FAIL_REQUIRE(Error, "Operation failed");
  }

  KJ_UNREACHABLE;
}

kj::Maybe<kj::String> X509Certificate::checkEmail(
    kj::String email, jsg::Optional<CheckOptions> options) {
  ClearErrorOnReturn clearErrorOnReturn;
  switch (X509_check_email(cert_.get(), email.begin(), email.size(), optionsToFlags(options))) {
    case 1:  // Match!
      return kj::mv(email);
    case 0:             // No Match!
      return kj::none;  // No return value is set
    case -2:            // Error!
      JSG_FAIL_REQUIRE(Error, "Invalid name");
    default:  // Error!
      JSG_FAIL_REQUIRE(Error, "Operation failed");
  }

  KJ_UNREACHABLE;
}

kj::Maybe<kj::String> X509Certificate::checkIp(kj::String ip, jsg::Optional<CheckOptions> options) {
  ClearErrorOnReturn clearErrorOnReturn;
  switch (X509_check_ip_asc(cert_.get(), ip.begin(), optionsToFlags(options))) {
    case 1:  // Match!
      return kj::mv(ip);
    case 0:             // No Match!
      return kj::none;  // No return value is set
    case -2:            // Error!
      JSG_FAIL_REQUIRE(Error, "Invalid IP");
    default:  // Error!
      JSG_FAIL_REQUIRE(Error, "Operation failed");
  }

  KJ_UNREACHABLE;
}

bool X509Certificate::checkIssued(jsg::Ref<X509Certificate> other) {
  ClearErrorOnReturn clearErrorOnReturn;
  return X509_check_issued(other->cert_.get(), cert_.get()) == X509_V_OK;
}

bool X509Certificate::checkPrivateKey(jsg::Ref<CryptoKey> privateKey) {
  JSG_REQUIRE(privateKey->getType() == "private"_kj, Error, "Invalid key type");
  return privateKey->verifyX509Private(cert_.get());
}

bool X509Certificate::verify(jsg::Ref<CryptoKey> publicKey) {
  JSG_REQUIRE(publicKey->getType() == "public"_kj, Error, "Invalid key type");
  return publicKey->verifyX509Public(cert_.get());
}

jsg::JsObject X509Certificate::toLegacyObject(jsg::Lock& js) {
  ClearErrorOnReturn clearErrorOnReturn;
  auto obj = js.obj();
  KJ_IF_SOME(subject, getX509NameObject<X509_get_subject_name>(js, cert_.get())) {
    obj.set(js, "subject", subject);
  }
  KJ_IF_SOME(issuer, getX509NameObject<X509_get_issuer_name>(js, cert_.get())) {
    obj.set(js, "issuer", issuer);
  }
  obj.set(js, "subjectAltName", js.str(getSubjectAltName().orDefault(kj::String())));
  obj.set(js, "infoAccess", js.str(getInfoAccess().orDefault(kj::String())));
  obj.set(js, "ca", js.boolean(getIsCA()));

  KJ_IF_SOME(key, getInnerPublicKey(cert_.get())) {
    auto bio = KJ_ASSERT_NONNULL(newBio());
    switch (EVP_PKEY_id(key.get())) {
      case EVP_PKEY_RSA: {
        RSA* rsa = EVP_PKEY_get0_RSA(key.get());
        KJ_ASSERT(rsa != nullptr);
        obj.set(js, "modulus", js.str(getModulusString(bio.get(), rsa->n)));
        obj.set(js, "bits", js.num(BN_num_bits(rsa->n)));
        obj.set(js, "exponent", js.str(getExponentString(bio.get(), rsa->e)));
        obj.set(js, "pubkey", jsg::JsValue(js.bytes(getRsaPubKey(rsa)).getHandle(js)));
        break;
      }
      case EVP_PKEY_EC: {
        EC_KEY* ec = EVP_PKEY_get0_EC_KEY(key.get());
        const EC_GROUP* group = EC_KEY_get0_group(ec);
        KJ_ASSERT(ec != nullptr);
        KJ_ASSERT(group != nullptr);
        KJ_IF_SOME(bits, getECGroupBits(group)) {
          obj.set(js, "bits", js.num(bits));
        }
        KJ_IF_SOME(pubkey, getECPubKey(group, ec)) {
          obj.set(js, "pubkey", jsg::JsValue(js.bytes(kj::mv(pubkey)).getHandle(js)));
        }

        const int nid = EC_GROUP_get_curve_name(group);
        if (nid != 0) {
          // Curve is well-known, get its OID and NIST nick-name (if it has one).

          KJ_IF_SOME(name, getCurveName<OBJ_nid2sn>(nid)) {
            obj.set(js, "asn1Curve", js.str(name));
          }
          KJ_IF_SOME(name, getCurveName<EC_curve_nid2nist>(nid)) {
            obj.set(js, "nistCurve", js.str(name));
          }
        } else {
          // Unnamed curves can be described by their mathematical properties,
          // but aren't used much (at all?) with X.509/TLS. Support later if needed.
        }
        break;
      }
    }
  }

  KJ_IF_SOME(from, getValidFrom()) {
    obj.set(js, "valid_from", js.str(from));
  }
  KJ_IF_SOME(to, getValidTo()) {
    obj.set(js, "valid_to", js.str(to));
  }

  KJ_IF_SOME(fingerprint, getFingerprint()) {
    obj.set(js, "fingerprint", js.str(fingerprint));
  }
  KJ_IF_SOME(fingerprint256, getFingerprint256()) {
    obj.set(js, "fingerprint256", js.str(fingerprint256));
  }
  KJ_IF_SOME(fingerprint512, getFingerprint512()) {
    obj.set(js, "fingerprint512", js.str(fingerprint512));
  }
  KJ_IF_SOME(keyUsage, getKeyUsage()) {
    auto values = KJ_MAP(str, keyUsage) { return jsg::JsValue(js.str(str)); };
    obj.set(js, "ext_key_usage", js.arr(values));
  }
  KJ_IF_SOME(serialNumber, getSerialNumber()) {
    obj.set(js, "serialNumber", js.str(serialNumber));
  }
  obj.set(js, "raw", jsg::JsValue(js.bytes(getRaw()).getHandle(js)));

  return obj;
}

}  // namespace workerd::api
