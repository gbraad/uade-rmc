import argparse
import bencode
import hashlib
from typevalidator import validate2, OPTIONAL_KEY, ONE_OR_MORE

import file_util


META_MAX_SIZE = 4096

META_FORMAT = {
    b'platform': b'amiga',
    b'subsongs': {int: int},  # length in milliseconds
    OPTIONAL_KEY(b'format'): bytes,
    OPTIONAL_KEY(b'title'): bytes,
    OPTIONAL_KEY(b'authors'): [ONE_OR_MORE, bytes],
    OPTIONAL_KEY(b'year'): bytes,
    OPTIONAL_KEY(b'song'): bytes,
    OPTIONAL_KEY(b'comment'): bytes,

    # Amiga specific
    OPTIONAL_KEY(b'player'): bytes,
    OPTIONAL_KEY(b'timer'): bytes,
    }

FILES_FORMAT = {bytes: bytes}


class RMC:
    def __init__(self, data: bytes = None):
        assert data is not None
        self.data = data
        self.li = bencode.bdecode(self.data)
        assert len(self.li) >= 3
        assert self.li[0] == b'rmc\x00\xfb\x13\xf6\x1f\xa2'
        self.validate()

    def validate(self):
        validate2(META_FORMAT, self.li[1])
        validate2(FILES_FORMAT, self.li[2])

    def get_meta(self):
        return self.li[1]

    def get_files(self):
        return self.li[2]

    def serialize(self):
        return bencode.bencode(self.li)


def _modland_check(fname, rmc, md5_to_meta):
    md5 = hashlib.md5()
    rmc_meta = rmc.get_meta()
    rmc_files = rmc.get_files()
    song_name = rmc_meta[b'song']
    song_bytes = rmc_files[song_name]
    assert isinstance(song_bytes, bytes)
    md5.update(song_bytes)
    digest = md5.hexdigest()
    modland_meta = md5_to_meta.get(digest)
    if modland_meta is not None:
        modland_authors = modland_meta[2]
        if rmc_meta.get(b'authors') is None and len(modland_authors) > 0:
            rmc_meta[b'authors'] = list(modland_authors)


def _process(fname, md5_to_meta):
    with open(fname, 'rb') as f:
        data = f.read()
        rmc = RMC(data)

    _modland_check(fname, rmc, md5_to_meta)
    rmc.validate()

    assert len(bencode.bencode(rmc.get_meta())) < META_MAX_SIZE

    new_data = rmc.serialize()
    if new_data != data:
        with file_util.AtomicReplacement(fname, 'wb') as f:
            print('Data changed', rmc.get_meta())
            f.write(new_data)

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
                    author = author[5:]
                    if author != 'Unknown':
                        authors.append(author.encode())
                else:
                    authors.append(author.encode())
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
