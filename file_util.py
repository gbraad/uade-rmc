# Copyright 2020 Heikki Orsila
#
# SPDX short identifier: BSD-2-Clause
#
# Read README.md for usage
#
# TODO: File locking support for applications that need mutual exclusion

import os
import tempfile


class AtomicReplacement:
    SUPPORTED_MODES = set(('w', 'w+', 'wb', 'w+b'))

    def __init__(self, fname, mode, **kwargs):
        if mode not in self.SUPPORTED_MODES:
            raise ValueError(
                'Invalid mode: {}. Please give one of {}.'.format(
                    mode, self.SUPPORTED_MODES))

        self._fname = fname
        self._mode = mode
        self._kwargs = kwargs
        self._f = None

    def _atomic_replace(self):
        # First copy the name so that temp file may delete the name
        tmpname = self._f.name

        # Close the tempfile so that we may use its content
        self.close()

        # Copy permissions from the target file if it exists
        if os.path.isfile(self._fname):
            st = os.stat(self._fname)
            os.chmod(tmpname, st.st_mode)
            os.chown(tmpname, st.st_uid, st.st_gid)

        # Atomically replace the target file with the temp file
        os.replace(tmpname, self._fname)

    def _discard(self):
        os.remove(self._f.name)

    def close(self):
        # Double close is valid
        if self._f is not None:
            self._f.close()
            self._f = None

    def open(self):
        if self._f is not None:
            raise AssertionError('Double open for file {}'.format(self._fname))

        dirname = os.path.dirname(self._fname)
        self._f = tempfile.NamedTemporaryFile(mode=self._mode,
                                              dir=dirname, delete=False)

    def __enter__(self):
        self.open()
        return self._f

    def __exit__(self, exc_type, unused_exc_value, unused_traceback):
        if exc_type is None:
            self._atomic_replace()
        else:
            # There was an exception in processing. Delete the temp file.
            self._discard()

        self.close()
