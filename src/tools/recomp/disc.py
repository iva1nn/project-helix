"""Read files from a PS1 disc image (.cue + MODE2/2352 .bin) through ISO9660.

Used at build time to fetch the exe and overlays from the user's own disc, so no game file has to be
copied into the repo.
"""
import re
import struct
from pathlib import Path

SECTOR = 2352
USER_OFS = 24  # MODE2 form 1: 12 sync + 4 header + 8 subheader


class Disc:
    def __init__(self, path):
        path = Path(path)
        if path.suffix.lower() == ".cue":
            m = re.search(r'FILE\s+"([^"]+)"', path.read_text(errors="replace"))
            path = path.parent / m.group(1)
        self.f = open(path, "rb")
        self._files = None

    def sector(self, lba):
        self.f.seek(lba * SECTOR + USER_OFS)
        return self.f.read(2048)

    def read(self, lba, size):
        out = bytearray()
        for i in range((size + 2047) // 2048):
            out += self.sector(lba + i)
        return bytes(out[:size])

    def files(self):
        """Map upper-case path without ';1' -> (lba, size)."""
        if self._files is None:
            pvd = self.sector(16)
            root = pvd[156:156 + 34]
            self._files = {}
            self._walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")
        return self._files

    def _walk(self, lba, size, prefix):
        data = self.read(lba, size)
        pos = 0
        while pos < len(data):
            n = data[pos]
            if n == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + n]
            elba, esize = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            flags, nlen = rec[25], rec[32]
            name = rec[33:33 + nlen]
            pos += n
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii", "replace").split(";")[0].upper()
            full = prefix + "/" + name
            if flags & 2:
                self._walk(elba, esize, full)
            else:
                self._files[full] = (elba, esize)

    def file(self, name):
        lba, size = self.files()[name.upper()]
        return self.read(lba, size)
