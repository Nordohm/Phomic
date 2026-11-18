#!/usr/bin/env bash
# One-time setup of App Store signing for .github/workflows/ios.yml. Runs anywhere with bash,
# openssl and tar (Git Bash on Windows too). Everything goes into a private folder outside the
# repo (default ~/phomic-signing): keep it, don't commit it. See docs/development.md, "App Store builds".
#
#   bash Tools/app-store-signing.sh csr
#       Makes a private key and a certificate signing request (request.csr). Upload the CSR at
#       developer.apple.com > Certificates > + > Apple Distribution, then download the .cer.
#   bash Tools/app-store-signing.sh p12 <downloaded .cer>
#       Makes distribution.p12 (with a random password) from the key and the certificate.
#   bash Tools/app-store-signing.sh profiles <app .mobileprovision> <widget .mobileprovision>
#       Copies in the two App Store provisioning profiles.
#   bash Tools/app-store-signing.sh secret <AuthKey_KEYID.p8> <issuer ID, or a file holding it>
#       Packs all of it plus the App Store Connect API key into secret.txt: the value of the one
#       GitHub secret APPLE_SIGNING (repository Settings > Secrets and variables > Actions).
set -euo pipefail
dir=${PHOMIC_SIGNING_DIR:-$HOME/phomic-signing}
mkdir -p "$dir"
chmod 700 "$dir" 2>/dev/null || true

# A value without whitespace or a UTF-8 byte order mark (as Notepad may save it).
clean() { sed 's/^\xEF\xBB\xBF//' | tr -d '[:space:]'; }

case "${1:-}" in
csr)
    if [ -e "$dir/distribution.key" ]; then
        echo "$dir/distribution.key already exists; delete it first to start over." >&2
        exit 1
    fi
    openssl genrsa -out "$dir/distribution.key" 2048
    # The subject comes from a config file: Git Bash would turn -subj "/CN=..." into a path.
    printf '[req]\nprompt = no\ndistinguished_name = dn\n[dn]\nCN = Phomic Distribution\n' > "$dir/csr.cnf"
    openssl req -new -key "$dir/distribution.key" -out "$dir/request.csr" -config "$dir/csr.cnf"
    echo "Upload $dir/request.csr as an Apple Distribution certificate, then run: $0 p12 <the .cer>"
    ;;
p12)
    cer=${2:?usage: $0 p12 <downloaded .cer>}
    password=$(openssl rand -hex 16)
    openssl x509 -inform DER -in "$cer" -out "$dir/distribution.pem"
    # 3DES/SHA-1 so macOS's security tool can read it (OpenSSL 3's default it can't).
    openssl pkcs12 -export -inkey "$dir/distribution.key" -in "$dir/distribution.pem" \
        -out "$dir/distribution.p12" -passout "pass:$password" \
        -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1
    printf '%s' "$password" > "$dir/p12-password"
    echo "Wrote $dir/distribution.p12"
    ;;
profiles)
    cp "${2:?usage: $0 profiles <app .mobileprovision> <widget .mobileprovision>}" "$dir/app.mobileprovision"
    cp "${3:?usage: $0 profiles <app .mobileprovision> <widget .mobileprovision>}" "$dir/widget.mobileprovision"
    echo "Copied the profiles to $dir"
    ;;
secret)
    p8=${2:?usage: $0 secret <AuthKey_KEYID.p8> <issuer ID, or a file holding it>}
    issuer=${3:?usage: $0 secret <AuthKey_KEYID.p8> <issuer ID, or a file holding it>}
    key_id=$(basename "$p8" | sed -n 's/^AuthKey_\([A-Z0-9]*\)\.p8$/\1/p')
    if [ -z "$key_id" ]; then echo "Expected the key file's original name, AuthKey_<key ID>.p8" >&2; exit 1; fi
    if [ -f "$issuer" ]; then issuer=$(clean < "$issuer"); else issuer=$(printf '%s' "$issuer" | clean); fi
    if ! printf '%s' "$issuer" | grep -q -E '^[0-9a-fA-F-]{36}$'; then
        echo "The issuer ID should look like 69a6de7e-...-...: got '$issuer'" >&2; exit 1
    fi
    for f in distribution.p12 p12-password app.mobileprovision widget.mobileprovision; do
        [ -e "$dir/$f" ] || { echo "Missing $dir/$f: run the p12 and profiles steps first." >&2; exit 1; }
    done
    cp "$p8" "$dir/AuthKey.p8"
    printf '%s' "$key_id" > "$dir/key-id"
    printf '%s' "$issuer" > "$dir/issuer-id"
    tar -czf "$dir/signing.tar.gz" -C "$dir" \
        distribution.p12 p12-password app.mobileprovision widget.mobileprovision AuthKey.p8 key-id issuer-id
    openssl base64 -A -in "$dir/signing.tar.gz" > "$dir/secret.txt"
    size=$(wc -c < "$dir/secret.txt")
    if [ "$size" -gt 48000 ]; then echo "secret.txt is $size bytes; GitHub secrets hold 48 KB at most." >&2; exit 1; fi
    echo "Wrote $dir/secret.txt ($size bytes): paste it as the GitHub secret APPLE_SIGNING."
    ;;
*)
    sed -n '2,15p' "$0"
    exit 1
    ;;
esac
