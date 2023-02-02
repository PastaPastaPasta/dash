#!/usr/bin/env python3
# Copyright (c) 2023 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import nacl.signing

# Generate a new EdDSA key pair
signing_key = nacl.signing.SigningKey.generate()

# Encode the secret and public keys as hexadecimal strings
secret_hex = signing_key.encode().hex()
public_hex = signing_key.verify_key.encode().hex()

# Output the key pair as a JSON object
print(f'{{\n  "secret": "{secret_hex}",\n  "public": "{public_hex}"\n}}')
