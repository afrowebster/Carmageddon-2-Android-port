"""Pure-Python APK assembly and signing (APK Signature Scheme v2), no external tools.

* ZipWriter: writes a zip with Android's alignment rules (stored entries 4-byte aligned, stored native
  libraries 16 KiB aligned so they can be mapped straight from the APK).
* make_key(): RSA-2048 key + self-signed X.509 certificate, generated with the standard library.
* sign_v2(): inserts an APK Signing Block (v2 scheme, RSA PKCS#1 v1.5 + SHA-256). Enough for minSdk 24.
"""
import hashlib, os, secrets, struct, time, zlib

# ----------------------------------------------------------------------------------------------- zip

class ZipWriter:
    def __init__(self, path):
        self.f = open(path, 'wb')
        self.entries = []          # (name, method, crc, csize, usize, offset, dostime)
        self.names = set()
        t = time.localtime()
        self.dostime = ((max(t.tm_year, 1980) - 1980) << 25 | t.tm_mon << 21 | t.tm_mday << 16
                        | t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2)

    def add(self, name, data, compress=True, align=4):
        if name in self.names:
            raise ValueError('duplicate zip entry ' + name)
        self.names.add(name)
        crc = zlib.crc32(data) & 0xffffffff
        if compress:
            co = zlib.compressobj(6, zlib.DEFLATED, -15)
            cdata = co.compress(data) + co.flush()
            if len(cdata) >= len(data):
                compress = False
        method = 8 if compress else 0
        if not compress:
            cdata = data
        bname = name.encode('utf-8')
        off = self.f.tell()
        extra = b''
        if method == 0:
            # pad the local header's extra field so the data starts on an `align` boundary
            data_start = off + 30 + len(bname)
            pad = (-data_start) % align
            if pad:
                while pad < 6:
                    pad += align
                # 0xd935: Android's alignment extra field (id, size, alignment, zero padding)
                extra = struct.pack('<HHH', 0xd935, pad - 4, align) + b'\0' * (pad - 6)
        flags = 0x800  # UTF-8 names
        self.f.write(struct.pack('<IHHHIIIIHH', 0x04034b50, 20, flags, method, self.dostime, crc,
                                 len(cdata), len(data), len(bname), len(extra)))
        self.f.write(bname + extra)
        self.f.write(cdata)
        self.entries.append((bname, method, crc, len(cdata), len(data), off, flags))

    def close(self):
        cd = self.f.tell()
        for bname, method, crc, csize, usize, off, flags in self.entries:
            self.f.write(struct.pack('<IHHHHIIIIHHHHHII', 0x02014b50, 20, 20, flags, method, self.dostime, crc,
                                     csize, usize, len(bname), 0, 0, 0, 0, 0, off))
            self.f.write(bname)
        end = self.f.tell()
        self.f.write(struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(self.entries), len(self.entries),
                                 end - cd, cd, 0))
        self.f.close()


# ----------------------------------------------------------------------------------------------- DER

def _len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b

def _tlv(tag, body):
    return bytes([tag]) + _len(len(body)) + body

def _int(v):
    b = v.to_bytes(v.bit_length() // 8 + 1, 'big')  # leading zero keeps it positive
    while len(b) > 1 and b[0] == 0 and b[1] < 0x80:
        b = b[1:]
    return _tlv(0x02, b)

def _seq(*items):
    return _tlv(0x30, b''.join(items))

def _oid(s):
    p = [int(x) for x in s.split('.')]
    out = bytes([40 * p[0] + p[1]])
    for v in p[2:]:
        enc = [v & 0x7f]
        v >>= 7
        while v:
            enc.append(0x80 | (v & 0x7f))
            v >>= 7
        out += bytes(reversed(enc))
    return _tlv(0x06, out)

NULL = b'\x05\x00'
RSA_ALG = _seq(_oid('1.2.840.113549.1.1.1'), NULL)
SHA256_RSA_ALG = _seq(_oid('1.2.840.113549.1.1.11'), NULL)
SHA256_DIGESTINFO = bytes.fromhex('3031300d060960864801650304020105000420')


# ----------------------------------------------------------------------------------------------- RSA

_SMALL_PRIMES = [p for p in range(3, 2000) if all(p % d for d in range(2, int(p ** 0.5) + 1))]

def _probable_prime(n):
    for p in _SMALL_PRIMES:
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(40):
        a = secrets.randbelow(n - 3) + 2
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True

def _prime(bits, e):
    while True:
        n = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if n % e != 1 and _probable_prime(n):
            return n

def _pubkey_info(n, e):
    return _seq(RSA_ALG, _tlv(0x03, b'\0' + _seq(_int(n), _int(e))))

def rsa_sign(key, msg):
    n, d = key['n'], key['d']
    k = (n.bit_length() + 7) // 8
    t = SHA256_DIGESTINFO + hashlib.sha256(msg).digest()
    em = b'\x00\x01' + b'\xff' * (k - len(t) - 3) + b'\x00' + t
    m = int.from_bytes(em, 'big')
    # CRT for speed
    p, q = key['p'], key['q']
    m1, m2 = pow(m, d % (p - 1), p), pow(m, d % (q - 1), q)
    h = (pow(q, -1, p) * (m1 - m2)) % p
    s = m2 + h * q
    assert pow(s, key['e'], n) == m
    return s.to_bytes(k, 'big')

def _gtime(t):
    return _tlv(0x18, time.strftime('%Y%m%d%H%M%SZ', time.gmtime(t)).encode())

def make_key(cn='Carmageddon 2 Android port (self-built)'):
    e = 65537
    while True:
        p, q = _prime(1024, e), _prime(1024, e)
        n = p * q
        if p != q and n.bit_length() == 2048:
            break
    d = pow(e, -1, (p - 1) * (q - 1))
    key = {'n': n, 'e': e, 'd': d, 'p': p, 'q': q}
    name = _seq(_tlv(0x31, _seq(_oid('2.5.4.3'), _tlv(0x0c, cn.encode()))))
    now = int(time.time())
    tbs = _seq(_tlv(0xa0, _int(2)), _int(secrets.randbits(63) | 1), SHA256_RSA_ALG, name,
               _seq(_gtime(now - 86400), _gtime(now + 30 * 365 * 86400)), name, _pubkey_info(n, e))
    key['cert'] = _seq(tbs, SHA256_RSA_ALG, _tlv(0x03, b'\0' + rsa_sign(key, tbs)))
    return key

def save_key(key, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.part'
    with open(tmp, 'w') as f:
        for k in ('n', 'e', 'd', 'p', 'q'):
            f.write('%s %x\n' % (k, key[k]))
        f.write('cert %s\n' % key['cert'].hex())
    os.replace(tmp, path)

def load_key(path):
    key = {}
    for line in open(path):
        k, v = line.split()
        key[k] = bytes.fromhex(v) if k == 'cert' else int(v, 16)
    return key


# ----------------------------------------------------------------------------------------------- v2

def _lp(b):
    return struct.pack('<I', len(b)) + b

def _chunk_digest(sections):
    """sections: list of callables yielding byte blocks; v2 digests 1 MiB chunks of each section"""
    digests = []
    for sec in sections:
        for chunk in sec():
            digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(chunk)) + chunk).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()

def sign_v2(unsigned, out, key):
    MB = 1 << 20
    with open(unsigned, 'rb') as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(size - 22)
        eocd = f.read(22)
        assert eocd[:4] == b'PK\x05\x06', 'zip has a comment or is not a zip'
        cd_size, cd_off = struct.unpack_from('<II', eocd, 12)
        f.seek(cd_off)
        cd = f.read(cd_size)

        def entries():
            f.seek(0)
            left = cd_off
            while left:
                b = f.read(min(MB, left))
                left -= len(b)
                yield b

        def blocks(b):
            return lambda: (b[i:i + MB] for i in range(0, len(b), MB))

        digest = _chunk_digest([entries, blocks(cd), blocks(eocd)])  # EOCD's CD offset = block start = cd_off
        ALG = 0x0103  # RSASSA-PKCS1-v1_5 with SHA2-256
        signed_data = (_lp(_lp(struct.pack('<I', ALG) + _lp(digest))) + _lp(_lp(key['cert'])) + _lp(b''))
        signer = (_lp(signed_data) + _lp(_lp(struct.pack('<I', ALG) + _lp(rsa_sign(key, signed_data))))
                  + _lp(_pubkey_info(key['n'], key['e'])))
        value = _lp(_lp(signer))
        pair = struct.pack('<QI', 4 + len(value), 0x7109871a) + value
        block_size = len(pair) + 8 + 16
        block = struct.pack('<Q', block_size) + pair + struct.pack('<Q', block_size) + b'APK Sig Block 42'

        tmp = out + '.part'
        with open(tmp, 'wb') as o:
            for b in entries():
                o.write(b)
            o.write(block)
            o.write(cd)
            o.write(eocd[:16] + struct.pack('<I', cd_off + len(block)) + eocd[20:])
    os.replace(tmp, out)
