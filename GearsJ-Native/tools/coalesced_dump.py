#!/usr/bin/env python3
"""Read a UE3 Xbox 360 Coalesced_<lang>.bin and print or search its ini files. Ours, MIT.

Layout (big-endian): u32 file count, then per file: FString name, u32 section count,
per section: FString name, u32 key count, per key: FString key, FString value.
FString: s32 length counting the terminator; > 0 = 8-bit chars, < 0 = UTF-16LE chars (measured on Judgment INT), 0 = empty.

  coalesced_dump.py Coalesced_INT.bin                 list files and section counts
  coalesced_dump.py Coalesced_INT.bin --grep MotionBlur   every key/section/file matching (case-insensitive)
  coalesced_dump.py Coalesced_INT.bin --file Xbox360-GearEngine.ini   dump one file as ini text
"""
import argparse, re, struct, sys


class Reader:
    def __init__(self, data):
        self.d, self.o = data, 0

    def u32(self):
        v = struct.unpack_from(">i", self.d, self.o)[0]
        self.o += 4
        return v

    def fstring(self):
        n = self.u32()
        if n == 0:
            return ""
        if n > 0:
            s = self.d[self.o:self.o + n]
            self.o += n
            return s.rstrip(b"\0").decode("latin-1")
        n = -n
        s = self.d[self.o:self.o + 2 * n]
        self.o += 2 * n
        return s.decode("utf-16-le").rstrip("\0")


def parse(data):
    r = Reader(data)
    files = []
    for _ in range(r.u32()):
        fname = r.fstring()
        sections = []
        for _ in range(r.u32()):
            sname = r.fstring()
            keys = [(r.fstring(), r.fstring()) for _ in range(r.u32())]
            sections.append((sname, keys))
        files.append((fname, sections))
    if r.o != len(data):
        print(f"warning: {len(data) - r.o} bytes left over after parsing", file=sys.stderr)
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bundle")
    ap.add_argument("--grep")
    ap.add_argument("--file")
    a = ap.parse_args()
    files = parse(open(a.bundle, "rb").read())
    if a.grep:
        rx = re.compile(a.grep, re.I)
        for fname, sections in files:
            for sname, keys in sections:
                for k, v in keys:
                    if rx.search(k) or rx.search(sname):
                        print(f"{fname} | [{sname}] {k}={v}")
    elif a.file:
        for fname, sections in files:
            if fname.lower().endswith(a.file.lower()):
                print(f"; {fname}")
                for sname, keys in sections:
                    print(f"[{sname}]")
                    for k, v in keys:
                        print(f"{k}={v}")
                    print()
    else:
        for fname, sections in files:
            print(f"{sum(len(k) for _, k in sections):6d} keys {len(sections):5d} sections  {fname}")
        print(f"{len(files)} files")


if __name__ == "__main__":
    main()
