# enwik9 word prediction with PPMd

OptimaTech Labs compressed the 1,000,000,000-byte enwik9 Wikipedia benchmark to **146,743,637 bytes including the offline decoder**. The decoder restored every byte on a 48 GB Apple M4 Pro. The output passed a direct comparison with the original file and matched its SHA-256. [Download v0.1.0](https://github.com/Optimatech-Labs-LLC/enwik9-word-ppmd/releases/tag/v0.1.0)

| enwik9 archive | Offline decoder ZIP | Total |
|---:|---:|---:|
| 146,587,079 bytes | 156,558 bytes | **146,743,637 bytes** |

## How it works

The encoder groups articles by Wikipedia categories, templates and titles, and records their original order. It builds a 65,536-entry dictionary and combines word and phrase predictions with PPMd byte probabilities. A small neural model learns as it goes from the preceding eight dictionary words. The decoder makes the same updates from text it has already recovered. The archive carries the dictionary and the information needed to put every article back in place.

Adding special handling for numbers, UTF-8 text and wiki markup reduced the complete package from 147,142,538 to 146,743,637 bytes, a saving of **398,901 bytes** against the same word model without those features. [Measurements](benchmark.json)

## Download and restore offline

The scored decoder runs on Apple Silicon macOS. It uses the system shell and tar. The restore needs no compiler, network connection, original corpus or pretrained model. In an empty directory, run:

```sh
release=https://github.com/Optimatech-Labs-LLC/enwik9-word-ppmd/releases/download/v0.1.0
curl -fLO "$release/enwik9-word-ppmd.pkg"
curl -fLO "$release/offline-decoder.zip"
curl -fLO "$release/SHA256SUMS"
shasum -a 256 -c SHA256SUMS
unzip -q offline-decoder.zip -d unpacked
./unpacked/spnumark-restore restore enwik9-word-ppmd.pkg enwik9
wc -c enwik9
shasum -a 256 enwik9
```

Expected output is **1,000,000,000 bytes**, SHA-256 `159b85351e5f76e60cbe32e04c677847a9ecba3adc79addab6f4c6c7aa3744bc`. With a copy of the original, run `cmp enwik9 /path/to/original/enwik9` for a direct comparison. [Reference corpus](https://mattmahoney.net/dc/textdata.html)

## Reproduce the archive

From a checkout of this release, build with Python 3 and Zig 0.15.2 on Apple Silicon macOS. The script builds the encoder, the native decoder and its offline ZIP. The measured preset uses an 8,000,000,000-byte PPMd pool, order 16, and a neural width of 128.

```sh
python3 build.py
./build/spnumark-words pack 8g-both 65536 /path/to/original/enwik9 rebuilt.pkg
shasum -a 256 rebuilt.pkg build/offline-decoder.zip
```

Compare the two hashes against the archive and decoder entries in [SHA256SUMS](SHA256SUMS). On the 48 GB M4 Pro the original benchmark took **34 min 33 s** to encode and **33 min 12 s** to restore and check. Peak resident memory was 10.50 GB during compression and 9.11 GB during restore. Other trials shared the machine. [Benchmark record](benchmark.json)

## Credits and license

OptimaTech Labs wrote the Wikipedia transforms, word and phrase predictors, online neural model, probability mixer and packaging. The release includes **Dmitry Shkarin’s PPMd variant I**, **Dmitry Subbotin’s range coder**, and **Igor Pavlov’s 7-Zip C implementation**. The public-domain PPMd files include local changes for the larger memory pool. OptimaTech Labs' code is licensed under GPLv3. The Wikipedia text carries its own terms. [Upstream source](https://github.com/ip7z/7zip/tree/25.01/C) · [Software license](LICENSE) · [Attribution and corpus terms](ATTRIBUTION.md)
