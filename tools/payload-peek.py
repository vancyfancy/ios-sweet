#!/usr/bin/env python3
"""Extract named partitions from an Android OTA payload.bin (full OTA ops:
REPLACE, REPLACE_XZ, REPLACE_BZ). A minimal standalone version of
payload_dumper for the exact op set our ROMs use.

Usage: payload-peek.py payload.bin OUT_DIR NAME[,NAME...]
"""
import bz2, hashlib, lzma, os, struct, sys

sys.path.append('/Users/maidar/Library/Python/3.9/lib/python/site-packages/payload_dumper')
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pb'))
import update_metadata_pb2 as um

REPLACE, REPLACE_BZ, ZERO, REPLACE_XZ = 0, 1, 6, 8


def main():
    payload, outdir, names = sys.argv[1], sys.argv[2], sys.argv[3].split(',')
    os.makedirs(outdir, exist_ok=True)
    f = open(payload, 'rb')
    assert f.read(4) == b'CrAU', 'not a payload'
    ver = struct.unpack('>Q', f.read(8))[0]
    msize = struct.unpack('>Q', f.read(8))[0]
    sig = struct.unpack('>I', f.read(4))[0]
    dam = um.DeltaArchiveManifest()
    dam.ParseFromString(f.read(msize))
    f.read(sig)  # metadata signature follows the manifest
    data_off = f.tell()
    block = dam.block_size
    for p in dam.partitions:
        if p.partition_name not in names:
            continue
        out = os.path.join(outdir, p.partition_name + '.img')
        with open(out, 'wb') as o:
            num = int(p.new_partition_info.size / block)
            print('%s: %d blocks, %d ops' % (p.partition_name, num, len(p.operations)))
            written = 0
            for i, op in enumerate(p.operations):
                f.seek(data_off + op.data_offset)
                raw = f.read(op.data_length)
                if op.data_sha256_hash:
                    assert hashlib.sha256(raw).digest() == op.data_sha256_hash, i
                data = raw
                dst = op.dst_extents[0].start_block * block
                dstbytes = sum(e.num_blocks for e in op.dst_extents) * block
                if op.type == REPLACE:
                    pass
                elif op.type == REPLACE_XZ:
                    data = lzma.LZMADecompressor().decompress(data)
                elif op.type == REPLACE_BZ:
                    data = bz2.BZ2Decompressor().decompress(data)
                elif op.type == ZERO:
                    data = b'\x00' * dstbytes
                else:
                    raise RuntimeError('unsupported op type %d in %s op%d'
                                       % (op.type, p.partition_name, i))
                assert len(data) == dstbytes, (i, len(data), dstbytes)
                o.seek(dst)
                o.write(data)
                written = max(written, dst + len(data))
            o.truncate(num * block)
        print('  -> %s (%d MiB)' % (out, written // 1048576))


if __name__ == '__main__':
    main()
