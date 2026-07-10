#!/usr/bin/env python3
"""Pack an ESP-IDF app binary into the aes-gw2 OTA image container.

Format (see aes-gw2/fwupdate/image.go / pack.go):
    [ firm_size(u32 LE) | fletcher32(u32 LE) | aes_iv(16) | ciphertext ]

The plaintext is 0xFF-padded to a 16-byte boundary, Fletcher32 is computed
over the padded plaintext, and the payload is AES-128-CBC encrypted with the
shared image key (components/lccore/include/aes_key.h — test key, replace
for production).

Usage: fwpack.py <app.bin> <out.blob>
"""
import os
import re
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEY_HEADER = os.path.join(
    os.path.dirname(__file__), "..", "components", "lccore", "include", "aes_key.h"
)


def read_key():
    text = open(KEY_HEADER).read()
    body = text[text.index("{") + 1 : text.index("}")]
    key = bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]{2}", body))
    assert len(key) == 16, f"expected 16 key bytes, got {len(key)}"
    return key


def fletcher32(data: bytes) -> int:
    s1 = s2 = 0
    for i in range(0, len(data) - 1, 2):
        w = data[i] | data[i + 1] << 8
        s1 = (s1 + w) % 0xFFFF
        s2 = (s2 + s1) % 0xFFFF
    return s2 << 16 | s1


def pack(plain: bytes, key: bytes) -> bytes:
    if len(plain) % 16:
        plain += b"\xff" * (16 - len(plain) % 16)
    iv = os.urandom(16)
    enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    ct = enc.update(plain) + enc.finalize()
    return struct.pack("<II", len(ct), fletcher32(plain)) + iv + ct


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    blob = pack(open(sys.argv[1], "rb").read(), read_key())
    open(sys.argv[2], "wb").write(blob)
    print(f"{sys.argv[2]}: {len(blob)} bytes")


if __name__ == "__main__":
    main()
