use aws_lc_rs::encoding::AsBigEndian;
use aws_lc_rs::rand::SystemRandom;
use aws_lc_rs::signature::ECDSA_P256K1_SHA256_FIXED;
use aws_lc_rs::signature::ECDSA_P256K1_SHA256_FIXED_SIGNING;
use aws_lc_rs::signature::EcdsaKeyPair;
use aws_lc_rs::signature::KeyPair;
use aws_lc_rs::signature::ParsedPublicKey;

#[cxx::bridge(namespace = "workerd::rust::aws_lc")]
mod ffi {
    extern "Rust" {
        fn validate_public(input: &[u8]) -> bool;
        fn validate_keypair(seckey: &[u8], pubkey: &[u8]) -> bool;
        fn generate_keypair() -> Vec<u8>;
        fn sign(seckey: &[u8], pubkey: &[u8], message: &[u8]) -> Vec<u8>;
        fn verify(pubkey: &[u8], message: &[u8], signature: &[u8]) -> bool;
    }
}

#[must_use]
pub fn validate_public(input: &[u8]) -> bool {
    ParsedPublicKey::new(&ECDSA_P256K1_SHA256_FIXED, input).is_ok()
}

#[must_use]
pub fn validate_keypair(seckey: &[u8], pubkey: &[u8]) -> bool {
    EcdsaKeyPair::from_private_key_and_public_key(
        &ECDSA_P256K1_SHA256_FIXED_SIGNING,
        seckey,
        pubkey,
    )
    .is_ok()
}

#[must_use]
pub fn generate_keypair() -> Vec<u8> {
    let Ok(keypair) = EcdsaKeyPair::generate(&ECDSA_P256K1_SHA256_FIXED_SIGNING) else {
        return Vec::new();
    };
    let Ok(seckey_bytes) = keypair.private_key().as_be_bytes() else {
        return Vec::new();
    };
    let pubkey_bytes = keypair.public_key().as_ref();
    let mut out = Vec::with_capacity(32 + 65);
    out.extend_from_slice(seckey_bytes.as_ref());
    out.extend_from_slice(pubkey_bytes);
    out
}

#[must_use]
pub fn sign(seckey: &[u8], pubkey: &[u8], message: &[u8]) -> Vec<u8> {
    let Ok(keypair) = EcdsaKeyPair::from_private_key_and_public_key(
        &ECDSA_P256K1_SHA256_FIXED_SIGNING,
        seckey,
        pubkey,
    ) else {
        return Vec::new();
    };
    // aws-lc-rs ignores the SecureRandom parameter (kept for ring compatibility); nonces come
    // from AWS-LC's internal CSPRNG.
    let rng = SystemRandom::new();
    let Ok(signature) = keypair.sign(&rng, message) else {
        return Vec::new();
    };
    signature.as_ref().to_vec()
}

#[must_use]
pub fn verify(pubkey: &[u8], message: &[u8], signature: &[u8]) -> bool {
    let Ok(parsed) = ParsedPublicKey::new(&ECDSA_P256K1_SHA256_FIXED, pubkey) else {
        return false;
    };
    parsed.verify_sig(message, signature).is_ok()
}
