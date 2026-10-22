# Sparkle EdDSA signing key for scripts/release.sh and scripts/release-windows.sh (sourced; bash).
#
# rn_sparkle_signer BIN APP_KEY
#   Sets SIGN=(sign_update ...) and KEY_SOURCE. The key comes from, in this order:
#     1. ED_KEY_FILE=<file> (environment)
#     2. ~/.config/replaynes/sparkle-ed25519.key, when it exists (docs/RELEASE.md "Signing key file")
#     3. the login keychain, account SPARKLE_ACCOUNT (default ReplayNES)
#   A key file lets the release scripts sign over ssh (the keychain is locked in an ssh session).
#   Whatever the source, the key's public half must equal APP_KEY (SUPublicEDKey), or it exits:
#   for a file the public key is derived from the private key with OpenSSL 3 (sign_update itself
#   only checks a signature against the same file).
RN_SPARKLE_KEY_FILE_DEFAULT="$HOME/.config/replaynes/sparkle-ed25519.key"

rn_sparkle_signer() {
  local bin="$1" app_key="$2" file="${ED_KEY_FILE:-}" key
  [ -z "$file" ] && [ -f "$RN_SPARKLE_KEY_FILE_DEFAULT" ] && file="$RN_SPARKLE_KEY_FILE_DEFAULT"
  if [ -n "$file" ]; then
    [ -f "$file" ] || { echo "ED_KEY_FILE $file does not exist"; exit 1; }
    case "$(stat -c %a "$file" 2>/dev/null || stat -f %Lp "$file")" in
      600|400) ;;
      *) echo "$file must be readable by its owner only (chmod 600)"; exit 1 ;;
    esac
    key="$(rn_sparkle_public_key "$file")" || { echo "cannot read an ed25519 key from $file"; exit 1; }
    SIGN=("$bin/sign_update" --ed-key-file "$file")
    KEY_SOURCE="key file $file"
  else
    key="$("$bin/generate_keys" --account "${SPARKLE_ACCOUNT:-ReplayNES}" -p)" ||
      { echo "no Sparkle key for account ${SPARKLE_ACCOUNT:-ReplayNES} in the keychain (over ssh the keychain is locked: use a key file, docs/RELEASE.md)"; exit 1; }
    SIGN=("$bin/sign_update" --account "${SPARKLE_ACCOUNT:-ReplayNES}")
    KEY_SOURCE="keychain account ${SPARKLE_ACCOUNT:-ReplayNES}"
  fi
  [ "$key" = "$app_key" ] || { echo "SUPublicEDKey ($app_key) != the signing key's public key ($key, $KEY_SOURCE)"; exit 1; }
  echo "==> signing key: $KEY_SOURCE"
}

# The base64 public key of a Sparkle private key file (base64 of the 32-byte seed; a 64-byte
# seed + public key or the legacy 96-byte form are accepted too).
rn_sparkle_public_key() {
  local openssl seedhex len
  openssl="$(command -v /opt/homebrew/bin/openssl || command -v openssl)"
  len="$(base64 -d < "$1" 2>/dev/null | wc -c | tr -d ' ')"
  case "$len" in
    32|64) seedhex="$(base64 -d < "$1" | head -c 32 | xxd -p -c 64)" ;;
    96) seedhex="$(base64 -d < "$1" | head -c 32 | xxd -p -c 64)" ;;
    *) return 1 ;;
  esac
  # PKCS#8 for an Ed25519 private key = fixed 16-byte prefix + the seed; the public key is the
  # last 32 bytes of the DER SubjectPublicKeyInfo.
  printf '302e020100300506032b657004220420%s' "$seedhex" | xxd -r -p |
    "$openssl" pkey -inform DER -pubout -outform DER 2>/dev/null | tail -c 32 | base64
}
