#pragma once

#include <workerd/jsg/jsg.h>

#include <openssl/x509.h>

namespace workerd::api {

class CryptoKey;

class X509Certificate: public jsg::Object {
 public:
  X509Certificate(X509* cert): cert_(kj::disposeWith<X509_free>(cert)) {}

  static kj::Maybe<jsg::Ref<X509Certificate>> parse(kj::Array<const kj::byte> raw);

  kj::Maybe<kj::String> getSubject();
  kj::Maybe<kj::String> getSubjectAltName();
  kj::Maybe<kj::String> getInfoAccess();
  kj::Maybe<kj::String> getIssuer();
  kj::Maybe<jsg::Ref<X509Certificate>> getIssuerCert();
  kj::Maybe<kj::String> getValidFrom();
  kj::Maybe<kj::String> getValidTo();
  kj::Maybe<kj::Array<kj::String>> getKeyUsage();
  kj::Maybe<kj::Array<const char>> getSerialNumber();
  jsg::BufferSource getRaw(jsg::Lock& js);
  kj::Maybe<jsg::Ref<CryptoKey>> getPublicKey();
  kj::Maybe<kj::String> getPem();
  kj::Maybe<kj::String> getFingerprint();
  kj::Maybe<kj::String> getFingerprint256();
  kj::Maybe<kj::String> getFingerprint512();
  bool getIsCA();

  struct CheckOptions {
    jsg::Optional<kj::String> subject;
    jsg::Optional<bool> wildcards;
    jsg::Optional<bool> partialWildcards;
    jsg::Optional<bool> multiLabelWildcards;
    jsg::Optional<bool> singleLabelSubdomains;
    JSG_STRUCT(subject, wildcards, partialWildcards, multiLabelWildcards, singleLabelSubdomains);
  };
  kj::Maybe<kj::String> checkHost(kj::String name, jsg::Optional<CheckOptions> options);

  kj::Maybe<kj::String> checkEmail(kj::String email, jsg::Optional<CheckOptions> options);

  kj::Maybe<kj::String> checkIp(kj::String ip, jsg::Optional<CheckOptions> options);

  bool checkIssued(jsg::Ref<X509Certificate> other);

  bool checkPrivateKey(jsg::Ref<CryptoKey> privateKey);

  bool verify(jsg::Ref<CryptoKey> publicKey);

  jsg::JsObject toLegacyObject(jsg::Lock& js);

  JSG_RESOURCE_TYPE(X509Certificate) {
    JSG_STATIC_METHOD(parse);
    JSG_READONLY_PROTOTYPE_PROPERTY(subject, getSubject);
    JSG_READONLY_PROTOTYPE_PROPERTY(subjectAltName, getSubjectAltName);
    JSG_READONLY_PROTOTYPE_PROPERTY(infoAccess, getInfoAccess);
    JSG_READONLY_PROTOTYPE_PROPERTY(issuer, getIssuer);
    JSG_READONLY_PROTOTYPE_PROPERTY(issuerCert, getIssuerCert);
    JSG_READONLY_PROTOTYPE_PROPERTY(validFrom, getValidFrom);
    JSG_READONLY_PROTOTYPE_PROPERTY(validTo, getValidTo);
    JSG_READONLY_PROTOTYPE_PROPERTY(fingerprint, getFingerprint);
    JSG_READONLY_PROTOTYPE_PROPERTY(fingerprint256, getFingerprint256);
    JSG_READONLY_PROTOTYPE_PROPERTY(fingerprint512, getFingerprint512);
    JSG_READONLY_PROTOTYPE_PROPERTY(keyUsage, getKeyUsage);
    JSG_READONLY_PROTOTYPE_PROPERTY(serialNumber, getSerialNumber);
    JSG_READONLY_PROTOTYPE_PROPERTY(pem, getPem);
    JSG_READONLY_PROTOTYPE_PROPERTY(raw, getRaw);
    JSG_READONLY_PROTOTYPE_PROPERTY(publicKey, getPublicKey);
    JSG_READONLY_PROTOTYPE_PROPERTY(isCA, getIsCA);
    JSG_METHOD(checkHost);
    JSG_METHOD(checkEmail);
    JSG_METHOD(checkIp);
    JSG_METHOD(checkIssued);
    JSG_METHOD(checkPrivateKey);
    JSG_METHOD(verify);
    JSG_METHOD(toLegacyObject);
  }

 private:
  kj::Own<X509> cert_;
  kj::Maybe<jsg::Ref<X509Certificate>> issuerCert_;
};

}  // namespace workerd::api

KJ_DECLARE_NON_POLYMORPHIC(X509);

#define EW_CRYPTO_X509_ISOLATE_TYPES api::X509Certificate, api::X509Certificate::CheckOptions
