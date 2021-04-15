import argparse
import ast
import bencode
import hashlib
import os
import subprocess
import tempfile
from typevalidator import validate2, OPTIONAL_KEY, ONE_OR_MORE


META_MAX_SIZE = 4096

META_FORMAT = {
    'platform': 'amiga',
    'subsongs': {int: int},  # length in milliseconds
    OPTIONAL_KEY('format'): str,
    OPTIONAL_KEY('title'): str,
    OPTIONAL_KEY('authors'): [ONE_OR_MORE, str],
    OPTIONAL_KEY('year'): int,
    OPTIONAL_KEY('song'): bytes,
    OPTIONAL_KEY('comment'): str,

    # Amiga specific
    OPTIONAL_KEY('player'): bytes,
    OPTIONAL_KEY('timer'): str,
    }

RMC_FORMAT = [bytes, META_FORMAT, dict]


def _modland_check(fname, rmc_meta, rmc_files, md5_to_meta):
    changed = False
    md5 = hashlib.md5()
    song_name = rmc_meta['song']
    song_bytes = rmc_files[song_name]
    assert isinstance(song_bytes, bytes)
    md5.update(song_bytes)
    digest = md5.hexdigest()
    modland_meta = md5_to_meta.get(digest)
    if modland_meta is not None:
        print(fname, modland_meta)
        modland_authors = modland_meta[2]
        if rmc_meta.get('authors', []) != modland_authors:
            rmc_meta['authors'] = list(modland_authors)
            changed = True

    return changed


def _process(fname, md5_to_meta):
    temp_dir = tempfile.TemporaryDirectory()
    with open(fname, 'rb') as f:
        data = f.read()
        cp = subprocess.run(['rmc', '-u', temp_dir.name, fname])
        assert cp.returncode == 0
        meta_name = os.path.join(temp_dir.name, 'meta')
        data = open(meta_name, 'rb').read()
        try:
            data = data.decode()
        except UnicodeDecodeError:
            print(fname, 'meta data can not be decoded into utf-8')
            return False
        try:
            meta = ast.literal_eval(data)
        except SyntaxError:  # Really!
            print(meta_name, 'is not in Python AST format')
            return False
        validate2(META_FORMAT, meta)

    files = {}
    files_dir = os.path.join(temp_dir.name, 'files')
    for objectbasename in os.listdir(files_dir):
        objectpathname = os.path.join(files_dir, objectbasename)
        files[objectbasename] = open(objectpathname, 'rb').read()

    if _modland_check(fname, meta, files, md5_to_meta):
        validate2(META_FORMAT, meta)
        print('meta changed', meta)

    assert len(bencode.bencode(meta)) < META_MAX_SIZE

    return True


def _parse_md5(fname):
    md5_to_meta = {}
    with open(fname, 'r') as f:
        for line in f.readlines():
            md5 = line[:32]
            modfname = line[33:].strip()
            assert (md5 + ' ' + modfname) == line.strip()
            fields = modfname.split('/')
            mod_format = fields[0]
            authors = []
            for author in fields[1:len(fields) - 1]:
                if author == '- unknown':
                    continue
                if author.startswith('coop-'):
                    authors.append(author[5:])
                else:
                    authors.append(author)
            md5_to_meta[md5] = (modfname, mod_format, authors)
    return md5_to_meta


def main():
    md5_to_meta = _parse_md5('allmods_md5.txt')

    parser = argparse.ArgumentParser()
    parser.add_argument('mods', nargs='*')
    args = parser.parse_args()
    for fname in args.mods:
        _process(fname, md5_to_meta)


if __name__ == '__main__':
    main()
