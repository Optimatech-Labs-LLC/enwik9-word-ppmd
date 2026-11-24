"""Build the measured Apple Silicon encoder, decoder and offline decoder ZIP."""

import argparse
import io
import lzma
from pathlib import Path
import subprocess
import tarfile
import tempfile
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile, ZipInfo


ROOT = Path(__file__).resolve().parent
PPMD = ROOT / "vendor" / "ppmd"
TARGET = "aarch64-macos.14.0"
FEATURES = [
    "-DWIKI_WIDE_PPM", "-DWIKI_DIGRAPH_FRAME", "-DWIKI_ONLINE_WORD_NET",
    "-ffp-contract=off", "-DWIKI_CONTENT_CONTEXT", "-DWIKI_NET_WIDTH=128",
    '-DWIKI_EXPERIMENT_MAGIC="WNMAR001"', "-DWIKI_EIGHT_WORDS", "-DWIKI_WORD_FINAL",
    "-DWIKI_RELATED_DICT", "-DWIKI_BIT_BUDGET", "-DWIKI_NUMERIC_UTF", "-DWIKI_MARKUP_CONTEXT",
]


def make_decoder_zip(native, output):
    executable = native.read_bytes()
    bundle = io.BytesIO()
    with tarfile.open(fileobj=bundle, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        entry = tarfile.TarInfo("decoder")
        entry.size = len(executable)
        entry.mode = 0o755
        tar.addfile(entry, io.BytesIO(executable))
    packed = lzma.compress(bundle.getvalue(), format=lzma.FORMAT_XZ,
                           preset=9 | lzma.PRESET_EXTREME)
    files = ROOT / "decoder"
    with ZipFile(output, "w") as archive:
        for name in ("spnumark-restore", "decoder.tar.xz", "CREDITS.txt", "USAGE.txt"):
            entry = ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (0o100755 if name == "spnumark-restore" else 0o100644) << 16
            if name == "decoder.tar.xz":
                archive.writestr(entry, packed, compress_type=ZIP_STORED)
            else:
                archive.writestr(entry, (files / name).read_bytes(),
                                 compress_type=ZIP_DEFLATED, compresslevel=9)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", default="zig", help="Zig 0.15.2 executable")
    parser.add_argument("--output", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([args.zig, "version"], text=True).strip()
    if version != "0.15.2":
        parser.error(f"The measured build uses Zig 0.15.2, found {version}")
    cc = [args.zig, "cc", "-target", TARGET, "-DWIKI_WIDE_PPM", "-std=c99", "-O3"]
    cxx = [args.zig, "c++", "-target", TARGET, *FEATURES, "-std=c++17", "-O3", "-s", "-I", PPMD]
    with tempfile.TemporaryDirectory(dir=output) as temp:
        objects = []
        for name in ("Ppmd8", "Ppmd8Dec", "Ppmd8Enc"):
            obj = Path(temp) / (name + ".o")
            subprocess.run([*cc, "-c", PPMD / (name + ".c"), "-o", obj], check=True)
            objects.append(obj)
        for name, flag, linked in (("spnumark-words", "-DWIKI_COHORT_AUDIT", objects),
                                   ("spnumark-restore", "-DDECODE_ONLY", objects[:2])):
            subprocess.run([*cxx, flag, ROOT / "src" / "digraph_words.cpp", *linked,
                            "-Wl,-dead_strip", "-o", output / name], check=True)
    make_decoder_zip(output / "spnumark-restore", output / "offline-decoder.zip")


if __name__ == "__main__":
    main()
