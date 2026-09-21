#!/usr/bin/env python3
"""Reference implementation of the Srun (深澜) portal login algorithm.

This is the source of the test vectors embedded in tests/test_main.cpp. It
exists so the C++ implementation in src/srun.cpp can be checked against an
independent transcription of the portal's JavaScript rather than against
itself. Run it to regenerate the vectors:

    python3 tests/srun_reference.py

The algorithm, as performed by the portal's main.js:

    info   = {"username":..,"password":..,"ip":..,"acid":..,"enc_ver":"srun_bx1"}
    i      = "{SRBX1}" + srun_base64(x_encode(json(info), token))
    hmd5   = HMAC-MD5(key=token, msg=password)
    chkstr = token+username + token+hmd5 + token+acid
           + token+ip + token+n + token+type + token+i
    chksum = SHA1(chkstr)
"""

import hashlib
import hmac
import json

ALPHABET = "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA"
MASK = 0xFFFFFFFF


def srun_base64(data: bytes) -> str:
    """Standard base64, but over the portal's own alphabet."""
    out = []
    imax = len(data) - len(data) % 3
    for i in range(0, imax, 3):
        n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2]
        out += [ALPHABET[(n >> 18) & 63], ALPHABET[(n >> 12) & 63],
                ALPHABET[(n >> 6) & 63], ALPHABET[n & 63]]
    rest = len(data) - imax
    if rest == 1:
        n = data[imax] << 16
        out += [ALPHABET[(n >> 18) & 63], ALPHABET[(n >> 12) & 63], "=", "="]
    elif rest == 2:
        n = (data[imax] << 16) | (data[imax + 1] << 8)
        out += [ALPHABET[(n >> 18) & 63], ALPHABET[(n >> 12) & 63],
                ALPHABET[(n >> 6) & 63], "="]
    return "".join(out)


def _to_words(data: bytes, append_length: bool):
    words = []
    for i in range(0, len(data), 4):
        n = 0
        for j in range(4):
            if i + j < len(data):
                n |= data[i + j] << (8 * j)
        words.append(n)
    if append_length:
        words.append(len(data))
    return words


def _to_bytes(words) -> bytes:
    out = bytearray()
    for w in words:
        out += bytes([w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF])
    return bytes(out)


def x_encode(data: bytes, key: bytes) -> bytes:
    """The XXTEA-like transform from the portal's JavaScript."""
    if not data:
        return b""

    v = _to_words(data, True)
    k = _to_words(key, False)
    while len(k) < 4:
        k.append(0)

    n = len(v) - 1
    z = v[n]
    c = 0x9E3779B9
    d = 0
    q = 6 + 52 // (n + 1)

    while q > 0:
        d = (d + c) & MASK
        e = (d >> 2) & 3
        for p in range(n):
            y = v[p + 1]
            m = ((z >> 5) ^ ((y << 2) & MASK)) & MASK
            m = (m + ((((y >> 3) ^ ((z << 4) & MASK)) ^ (d ^ y)) & MASK)) & MASK
            m = (m + ((k[(p & 3) ^ e] ^ z) & MASK)) & MASK
            v[p] = (v[p] + m) & MASK
            z = v[p]
        p = n
        y = v[0]
        m = ((z >> 5) ^ ((y << 2) & MASK)) & MASK
        m = (m + ((((y >> 3) ^ ((z << 4) & MASK)) ^ (d ^ y)) & MASK)) & MASK
        m = (m + ((k[(p & 3) ^ e] ^ z) & MASK)) & MASK
        v[n] = (v[n] + m) & MASK
        z = v[n]
        q -= 1

    return _to_bytes(v)


def build(username, password, ip, acid, token, n="200", type_="1"):
    info = json.dumps(
        {"username": username, "password": password, "ip": ip,
         "acid": acid, "enc_ver": "srun_bx1"},
        separators=(",", ":"),
    )
    i = "{SRBX1}" + srun_base64(x_encode(info.encode(), token.encode()))
    hmd5 = hmac.new(token.encode(), password.encode(), hashlib.md5).hexdigest()
    chkstr = (token + username + token + hmd5 + token + acid + token + ip +
              token + n + token + type_ + token + i)
    return {
        "info_json": info,
        "i": i,
        "hmd5": hmd5,
        "chksum": hashlib.sha1(chkstr.encode()).hexdigest(),
    }


def build_info_param_for_test(username, password, ip, acid, token):
    """Server-side recomputation of the `info` parameter, for tests/fake_portal.py."""
    return build(username, password, ip, acid, token)["i"]


def srun_hmd5(password, token):
    return hmac.new(token.encode(), password.encode(), hashlib.md5).hexdigest()


def srun_chksum(token, username, hmd5, acid, ip, n, type_, info_param):
    chkstr = (token + username + token + hmd5 + token + acid + token + ip +
              token + n + token + type_ + token + info_param)
    return hashlib.sha1(chkstr.encode()).hexdigest()


if __name__ == "__main__":
    # Fixed, obviously-fake inputs so the vectors are stable and safe to commit.
    v = build(username="20210001", password="s3cr3t p@ss", ip="10.253.51.53",
              acid="1", token="abcdef0123456789abcdef0123456789")

    print("base64 'hello'        :", srun_base64(b"hello"))
    print("base64 'hi'           :", srun_base64(b"hi"))
    print("base64 'a'            :", srun_base64(b"a"))
    print("x_encode hex          :", x_encode(b"hello world", b"key").hex())
    print("info_json             :", v["info_json"])
    print("i                     :", v["i"])
    print("hmd5                  :", v["hmd5"])
    print("chksum                :", v["chksum"])
