#!/bin/sh
# Makes the .7z fixtures of update_logic_test.cpp (committed, so the test needs no 7-Zip). Needs 7-Zip 23+ / p7zip:
#   SEVENZIP=/path/to/7za ./make.sh
# solid-lzma2.7z = the release settings (job 1406); bcj2.7z = a BCJ2 filter chain (7-Zip's choice for exe/dll);
# corrupt.7z = solid-lzma2.7z with byte 200 flipped. The test knows the files' SHA-256s (fixed seed).
set -e
Z=${SEVENZIP:-7za}
OUT=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; cd "$W"
mkdir -p COGBox-0.4.9/licenses COGBox-0.4.9/emptydir
python3 - <<'P'
import random
random.seed(49)
open('COGBox-0.4.9/CogBox.exe','wb').write(b'MZ'+bytes(random.getrandbits(8) for _ in range(3000))+b'\xe8\x00\x10\x00\x00'*400)
open('COGBox-0.4.9/README.txt','w').write('COGBox 0.4.9 test archive\n'*200)
open('COGBox-0.4.9/licenses/ümläut-テスト.txt','w',encoding='utf-8').write('unicode name\n')
open('COGBox-0.4.9/empty.bin','wb').close()
P
rm -f "$OUT"/solid-lzma2.7z "$OUT"/bcj2.7z "$OUT"/corrupt.7z
$Z a -t7z -m0=lzma2 -mx=9 -md=256m -ms=on "$OUT"/solid-lzma2.7z COGBox-0.4.9 >/dev/null
$Z a -t7z -m0=BCJ2 -m1=LZMA2:d=1m -m2=LZMA:d=64k -m3=LZMA:d=64k -mb0:1 -mb0s1:2 -mb0s2:3 -mx=9 -ms=on "$OUT"/bcj2.7z COGBox-0.4.9 >/dev/null
cp "$OUT"/solid-lzma2.7z "$OUT"/corrupt.7z
python3 -c "import sys;p=sys.argv[1];b=bytearray(open(p,'rb').read());b[200]^=0x55;open(p,'wb').write(b)" "$OUT"/corrupt.7z
