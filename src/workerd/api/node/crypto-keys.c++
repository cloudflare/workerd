#include "crypto.h"

namespace workerd::api::node {

kj::OneOf<kj::String, kj::Array<kj::byte>, SubtleCrypto::JsonWebKey> CryptoImpl::exportKey(
    jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::Optional<KeyExportOptions> options) {
  KJ_UNIMPLEMENTED("not implemented");
}

bool CryptoImpl::equals(jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Ref<CryptoKey> otherKey) {
  KJ_UNIMPLEMENTED("not implemented");
}

CryptoImpl::AsymmetricKeyDetails CryptoImpl::getAsymmetricKeyDetail(
    jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  KJ_UNIMPLEMENTED("not implemented");
}

kj::StringPtr CryptoImpl::getAsymmetricKeyType(jsg::Lock& js, jsg::Ref<CryptoKey> key) {
  KJ_UNIMPLEMENTED("not implemented");
}

CryptoKeyPair CryptoImpl::generateKeyPair(
    jsg::Lock& js,
    kj::String type,
    CryptoImpl::GenerateKeyPairOptions options) {
  KJ_UNIMPLEMENTED("not implemented");
}

jsg::Ref<CryptoKey> CryptoImpl::createSecretKey(jsg::Lock& js, kj::Array<kj::byte>) {
  KJ_UNIMPLEMENTED("not implemented");
}

jsg::Ref<CryptoKey> CryptoImpl::createPrivateKey(
    jsg::Lock& js,
    CreateAsymmetricKeyOptions options) {
  KJ_UNIMPLEMENTED("not implemented");
}

jsg::Ref<CryptoKey> CryptoImpl::createPublicKey(
    jsg::Lock& js,
    CreateAsymmetricKeyOptions options) {
  KJ_UNIMPLEMENTED("not implemented");
}

}  // namespace workerd::api::node
