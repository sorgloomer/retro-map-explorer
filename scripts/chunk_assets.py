#!/usr/bin/env python3
"""Splits large files (the encrypted asset zips) into fixed-size parts for hosts without Range or ETag support.

    python scripts/chunk_assets.py [--chunk-size BYTES] [--force] [FILE ...]

For every FILE (default: assets/ctdr2k.zip and assets/rvlt.zip) a folder "<FILE>.parts" is written:

    assets/ctdr2k.zip.parts/meta.json
    assets/ctdr2k.zip.parts/parts.json
    assets/ctdr2k.zip.parts/ctdr2k000.bin   bytes [0, chunksize) of the file
    assets/ctdr2k.zip.parts/ctdr2k001.bin   bytes [chunksize, 2 * chunksize) ...

    meta.json:  {"etag": "...", "chunksize": 4194304, "totalsize": 540698992, "partcount": 129}
    parts.json: {"parts": [{"name": "ctdr2k000.bin", "sha256": "..."}, {"name": "ctdr2k001.bin", "sha256": "..."}, ...]}

The last part is shorter. Part files are numbered with at least three digits (more when the file needs more than 1000
parts). "etag" is the first 16 hex digits of the SHA-256 of the whole file, so it only changes when the content does; parts.json
gives every part its own SHA-256, which readers use to verify downloads and to tell which cached parts are still valid. An
up-to-date folder is left untouched, otherwise it is rebuilt from scratch (written next to it and swapped in).
"""
import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FILES = [os.path.join(ROOT, 'assets', 'ctdr2k.zip'), os.path.join(ROOT, 'assets', 'rvlt.zip')]
DEFAULT_CHUNK = 4 * 1024 * 1024


def remove_tree(path):
    """rmtree that retries (Windows can briefly lock freshly written or served files) and fails loudly."""
    def onerror(func, p, exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    for attempt in range(10):
        if not os.path.exists(path):
            return
        try:
            shutil.rmtree(path, onerror=onerror)
        except OSError:
            time.sleep(0.3)
    if os.path.exists(path):
        raise SystemExit('cannot remove %s (is something using the files?)' % path)


def part_name(stem, index, parts):
    digits = max(3, len(str(parts - 1)))
    return '%s%0*d.bin' % (stem, digits, index)


def chunk_file(path, chunk_size, force):
    size = os.path.getsize(path)
    if size == 0:
        raise SystemExit('%s is empty' % path)
    parts = (size + chunk_size - 1) // chunk_size
    out = path + '.parts'
    stem = os.path.splitext(os.path.basename(path))[0]  # ctdr2k.zip -> ctdr2k: parts are ctdr2k000.bin, ctdr2k001.bin, ...
    tmp = out + '.tmp'
    remove_tree(tmp)
    os.makedirs(tmp)
    whole = hashlib.sha256()
    chunks = []
    with open(path, 'rb') as src:
        for i in range(parts):
            data = src.read(chunk_size)
            whole.update(data)
            name = part_name(stem, i, parts)
            with open(os.path.join(tmp, name), 'wb') as dst:
                dst.write(data)
            chunks.append({'name': name, 'sha256': hashlib.sha256(data).hexdigest()})
    meta = {'etag': whole.hexdigest()[:16], 'chunksize': chunk_size, 'totalsize': size, 'partcount': parts}
    partlist = {'parts': chunks}
    try:
        with open(os.path.join(out, 'meta.json'), encoding='utf-8') as f:
            old = (json.load(f), None)
        with open(os.path.join(out, 'parts.json'), encoding='utf-8') as f:
            old = (old[0], json.load(f))
    except (OSError, ValueError):
        old = None
    if old == (meta, partlist) and not force:
        remove_tree(tmp)
        print('%s: up to date (%d parts, etag %s)' % (os.path.relpath(out, ROOT), parts, meta['etag']))
        return
    for name, doc in (('meta.json', meta), ('parts.json', partlist)):
        with open(os.path.join(tmp, name), 'w', encoding='utf-8') as f:
            json.dump(doc, f, separators=(',', ':'))
            f.write('\n')
    remove_tree(out)
    os.rename(tmp, out)
    print('%s: %d parts of %d bytes, etag %s' % (os.path.relpath(out, ROOT), parts, chunk_size, meta['etag']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*', default=DEFAULT_FILES, help='files to split (default: the asset zips)')
    ap.add_argument('--chunk-size', type=int, default=DEFAULT_CHUNK, help='bytes per part (default 4194304 = 4 MiB)')
    ap.add_argument('--force', action='store_true', help='rewrite even when the folder is up to date')
    args = ap.parse_args()
    if args.chunk_size < 1:
        ap.error('--chunk-size must be positive')
    for f in args.files:
        if not os.path.isfile(f):
            print('skipping %s: no such file' % f, file=sys.stderr)
            continue
        chunk_file(os.path.abspath(f), args.chunk_size, args.force)


if __name__ == '__main__':
    main()
