#pragma once

/* SlipStream OTA image verification key (ECDSA-P256, SubjectPublicKeyInfo
 * PEM). This is a trust ANCHOR, not a secret -- it's the public half of
 * slipstream-web's OTA signing keypair (generated via
 * scripts/sign_firmware.py generate-key on the deployed server), used only
 * to verify that a downloaded firmware image was signed by that key.
 * Deliberately tracked in git (unlike secrets.h) so the trust root is
 * visible/auditable in source, not hidden in a gitignored file.
 *
 * To rotate: regenerate the keypair server-side, re-sign all published
 * images with the new key, and update this constant to match -- a
 * firmware build with the old key embedded won't accept images signed by
 * a rotated key until it's updated to this new constant and reflashed
 * (via OTA itself, using the last image signed by the OLD key, before the
 * old key is retired). */
static const char k_ota_trust_key_pem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEFDDTI7mzQHVWlX7r2q7lt7KcHNto\n"
    "zwQndO+cHmF1VRf6k8ieJhBDZ7QOqeBjHa2PPG0U0e+NgmWPrUUvOowBaQ==\n"
    "-----END PUBLIC KEY-----\n";
