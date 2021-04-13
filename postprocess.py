import argparse
import bencode
import hashlib
import subprocess
from typevalidator import validate2, OPTIONAL_KEY


META_FORMAT = {
    b'platform': b'amiga',
    b'subsongs': {int: int},  # length in milliseconds
    OPTIONAL_KEY(b'format'): str,
    OPTIONAL_KEY(b'subformat'): str,
    OPTIONAL_KEY(b'title'): str,
    OPTIONAL_KEY(b'author'): str,
    OPTIONAL_KEY(b'year'): int,
    OPTIONAL_KEY(b'song'): bytes,
    OPTIONAL_KEY(b'player'): str,
    OPTIONAL_KEY(b'epoption'): {},  # a dictionary of eagleplayer options
    OPTIONAL_KEY(b'comment'): str,
    }

RMC_FORMAT = [bytes, META_FORMAT, dict]


def _modland_check(fname, rmc, md5_to_meta):
    annotation = {}
    md5 = hashlib.md5()
    song = rmc[1][b'song']
    song_bytes = rmc[2][song]
    assert isinstance(song_bytes, bytes)
    md5.update(song_bytes)
    digest = md5.hexdigest()
    modland_meta = md5_to_meta.get(digest)
    if modland_meta is not None:
        print(fname, modland_meta)

    return len(annotation) > 0


def _process(fname, md5_to_meta):
    with open(fname, 'rb') as f:
        data = f.read()
        try:
            rmc = bencode.bdecode(data)
        except ValueError:
            print(fname, 'is not in rmc format (invalid bencode)')
            return False
        validate2(RMC_FORMAT, rmc)
        cp = subprocess.run(['rmc', '-u', fname])
        assert cp.returncode == 0

    _modland_check(fname, rmc, md5_to_meta)
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
