# License signing keys

`dev_private_key.pem` / `dev_public_key.pem` are a **development-only** Ed25519 key pair.
The public half is embedded in development builds (`Shared/Licensing/License.cpp`) so the
license verification path can be exercised in tests. It is intentionally public and grants
nothing in production.

Production builds must embed the ParsFlash production public key
(`-DPF_LICENSE_PUBLIC_KEY_FILE=<pem>`); the production private key is never stored in
this repository. See Docs/SECURITY.md.
