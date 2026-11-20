#!/usr/bin/env python3
"""Check the firmware's real interrupted-file validator against recovered audio."""
import argparse
import ctypes
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('audio_directory', type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]/'firmware/wifi-provision/main'
    with tempfile.TemporaryDirectory(prefix='recording-integrity-') as td:
        tmp = Path(td)
        library = tmp/'check.dylib'
        subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(source/'recording_integrity.c'), '-o', str(library)], check=True)
        check = ctypes.CDLL(str(library)).recording_file_complete
        check.argtypes = [ctypes.c_char_p]
        check.restype = ctypes.c_bool
        files = sorted(args.audio_directory.glob('*.opus*'))
        assert files
        for file in files:
            assert check(bytes(file)), file.name
        original = files[0].read_bytes()
        for n in (0, 1, 26, 28, 100, len(original)//2, len(original)-1):
            file = tmp/'truncated.opus'
            file.write_bytes(original[:n])
            assert not check(bytes(file)), n
        for n in (0, 5, 14, 18, 22, 100, len(original)-1):
            data = bytearray(original)
            data[n] ^= 1
            file = tmp/'corrupt.opus'
            file.write_bytes(data)
            assert not check(bytes(file)), n
        file = tmp/'extra-data.opus'
        file.write_bytes(original+b'junk')
        assert not check(bytes(file))
        assert not check(bytes(tmp/'missing.opus'))
        print(f'PASS: {len(files)} real complete Ogg files including interrupted .part; truncation, CRC/sequence corruption and extra data rejected')


if __name__ == '__main__':
    main()
