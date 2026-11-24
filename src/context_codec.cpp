// Standalone text codec. The preceding four or eight bytes select a previous
// continuation. Both sides compute the reference address from reconstructed text.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using std::size_t;
using Bytes = std::string;
using View = std::string_view;
constexpr unsigned unit = 4096, lower = 1u << 23, contexts = 4096, alphabet = 288;
constexpr size_t blockTokens = 1 << 18, limit = size_t{2} << 30;
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

Bytes readFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    auto n = file.tellg();
    check(n >= 0 && uint64_t(n) <= limit, "Input missing or too large");
    Bytes data(size_t(n), '\0');
    file.seekg(0);
    check(bool(file.read(data.data(), data.size())), "Read failed");
    return data;
}
void put(Bytes& out, uint32_t n) {
    while (n >= 128) { out.push_back(char((n & 127) | 128)); n >>= 7; }
    out.push_back(char(n));
}
struct Reader {
    View data;
    size_t at = 0;
    View take(size_t n) {
        check(n <= data.size() - at, "Truncated stream");
        View v = data.substr(at, n);
        at += n;
        return v;
    }
    uint32_t byte() { return uint8_t(take(1)[0]); }
    uint32_t integer() {
        uint64_t n = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7) {
            auto b = byte();
            n |= uint64_t(b & 127) << shift;
            if (b < 128) { check(n <= UINT32_MAX, "Integer overflow"); return uint32_t(n); }
        }
        throw std::runtime_error("Invalid integer");
    }
};

// Explicit little-endian reads make the lookup identical on every host.
uint32_t le32(const char* p) {
    auto b = reinterpret_cast<const uint8_t*>(p);
    return b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
unsigned hash4(const char* p) { return (le32(p) * 2654435761u) >> 10; }
unsigned hash8(const char* p) {
    uint64_t x = le32(p) | (uint64_t(le32(p + 4)) << 32);
    return unsigned(((x ^ (x >> 29)) * UINT64_C(11400714819323198485)) >> 41);
}
struct Prediction {
    uint32_t from[2];
    unsigned context;
};
struct History {
    std::vector<uint32_t> four = std::vector<uint32_t>(1 << 22);
    std::vector<uint32_t> eight = std::vector<uint32_t>(1 << 23);
    Prediction predict(const char* data, size_t pos) const {
        uint32_t shortPos = pos >= 4 ? four[hash4(data + pos - 4)] : 0;
        uint32_t longPos = pos >= 8 ? eight[hash8(data + pos - 8)] : 0;
        if (shortPos && le32(data + shortPos - 4) != le32(data + pos - 4)) shortPos = 0;
        if (longPos && std::memcmp(data + longPos - 8, data + pos - 8, 8)) longPos = 0;
        uint32_t pair = pos ? uint8_t(data[pos - 1]) : 0;
        if (pos >= 2) pair |= uint32_t(uint8_t(data[pos - 2])) << 8;
        unsigned context = ((pair * 2654435761u) >> 21) * 2 + unsigned(longPos != 0);
        return {{shortPos, longPos}, context};
    }
    void learn(const char* data, size_t at, size_t count) {
        for (size_t i = at; i < at + count; ++i) {
            if (i >= 4) four[hash4(data + i - 4)] = uint32_t(i);
            if (i >= 8) eight[hash8(data + i - 8)] = uint32_t(i);
        }
    }
};

using Row = std::array<uint16_t, alphabet + 1>;
using Model = std::vector<Row>;
struct Token { uint16_t context, symbol; };

#ifndef DECODE_ONLY
template<class Emit>
void parseAt(View src, Emit emit) {
    History history;
    for (size_t i = 0; i < src.size();) {
        auto p = history.predict(src.data(), i);
        size_t best = 0;
        unsigned route = 0;
        size_t max = std::min<size_t>(65535, src.size() - i);
        for (int j = 1; j >= 0; --j) {
            auto ref = p.from[j];
            if (!ref || (j == 0 && ref == p.from[1])) continue;
            size_t n = 0;
            while (n + 8 <= max && !std::memcmp(src.data()+ref+n, src.data()+i+n, 8)) n += 8;
            while (n < max && src[ref+n] == src[i+n]) ++n;
            if (n > best) { best = n; route = j; }
        }
        unsigned symbol, extra = 0, extraBits = 0;
        if (best >= 3) {
            unsigned x = unsigned(best) - 2;
            extraBits = 31 - __builtin_clz(x);
            symbol = 256 + route * 16 + extraBits;
            extra = x - (1u << extraBits);
        } else {
            symbol = uint8_t(src[i]);
            best = 1;
        }
        emit(Token{uint16_t(p.context), uint16_t(symbol)}, extra, extraBits, i, best, p);
        history.learn(src.data(), i, best);
        i += best;
    }
}

template<class Emit>
void parse(View src, Emit emit) {
    parseAt(src, [&](Token t, unsigned x, unsigned k, size_t, size_t, Prediction) { emit(t, x, k); });
}

template<class Parser>
Bytes encodeUsing(View src, View magic, Parser scan) {
    std::vector<std::array<uint32_t, alphabet>> counts(contexts);
    uint64_t copied = 0, references = 0;
    scan(src, [&](Token t, unsigned x, unsigned bits) {
        ++counts[t.context][t.symbol];
        if (t.symbol >= 256) { ++references; copied += 2 + (1u << bits) + x; }
    });
    Bytes archive(magic);
    put(archive, uint32_t(src.size()));
    Model model(contexts);
    for (unsigned k = 0; k < contexts; ++k) {
        uint64_t total = 0, cumulative = 0;
        unsigned active = 0, last = 0;
        for (uint32_t n : counts[k]) { total += n; active += n != 0; }
        put(archive, active);
        for (unsigned s = 0; s < alphabet; ++s) {
            unsigned f = 0;
            if (auto n = counts[k][s]) {
                uint64_t scale = unit - active;
                f = unsigned((cumulative + n) * scale / total - cumulative * scale / total) + 1;
                put(archive, s - last);
                put(archive, f);
                last = s + 1;
                cumulative += n;
            }
            model[k][s+1] = uint16_t(model[k][s]+f);
        }
    }
    size_t header = archive.size();
    std::vector<Token> block;
    block.reserve(blockTokens);
    Bytes extra;
    uint32_t pending = 0;
    unsigned bits = 0;
    auto flush = [&] {
        if (block.empty()) return;
        Bytes digits;
        uint32_t state = lower;
        for (auto it = block.rbegin(); it != block.rend(); ++it) {
            const auto& row = model[it->context];
            uint32_t start = row[it->symbol], f = row[it->symbol+1] - start;
            while (state >= (lower >> 12) * 256 * f) { digits.push_back(char(state & 255)); state >>= 8; }
            state = (state / f << 12) + state % f + start;
        }
        put(archive, uint32_t(block.size()));
        put(archive, uint32_t(digits.size() + 4));
        if (bits) extra.push_back(char(pending));
        put(archive, uint32_t(extra.size()));
        for (unsigned shift = 0; shift < 32; shift += 8) archive.push_back(char((state >> shift) & 255));
        archive.append(digits.rbegin(), digits.rend());
        archive.append(extra);
        block.clear(); extra.clear(); bits = 0; pending = 0;
    };
    scan(src, [&](Token t, unsigned x, unsigned n) {
        block.push_back(t);
        pending |= x << bits;
        bits += n;
        while (bits >= 8) { extra.push_back(char(pending & 255)); pending >>= 8; bits -= 8; }
        if (block.size() == blockTokens) flush();
    });
    flush();
    std::cerr << "Reference tokens: " << references << ", bytes copied: " << copied
              << ", included model header: " << header << '\n';
    return archive;
}
Bytes encode(View src) {
    return encodeUsing(src, "WCP1", [](View v, auto emit) { parse(v, emit); });
}
#endif

template<class Literal, class Observe>
Bytes restoreUsing(View archive, View magic, Literal literal, Observe observe) {
    Reader input{archive};
    check(input.take(4) == magic, "Invalid text archive");
    size_t size = input.integer();
    check(size <= limit, "Oversized output");
    Model model(contexts);
    std::vector<uint16_t> lookup(size_t(contexts) * unit);
    std::vector<bool> populated(contexts);
    for (unsigned k = 0; k < contexts; ++k) {
        uint32_t active = input.integer();
        check(active <= alphabet, "Too many symbols");
        populated[k] = active != 0;
        std::array<unsigned, alphabet> freq{};
        size_t next = 0;
        for (unsigned i = 0; i < active; ++i) {
            next += input.integer();
            check(next < alphabet, "Invalid symbol");
            freq[next] = input.integer();
            check(freq[next] && freq[next] <= unit, "Invalid frequency");
            ++next;
        }
        unsigned used = 0;
        for (unsigned s = 0; s < alphabet; ++s) {
            check(freq[s] <= unit - used, "Invalid cumulative frequency");
            auto p = lookup.begin() + size_t(k) * unit + used;
            std::fill(p, p + freq[s], uint16_t(s));
            used += freq[s];
            model[k][s+1] = uint16_t(used);
        }
        check(used == (active ? unit : 0), "Bad frequency table");
    }
    Bytes out(size, '\0');
    History history;
    size_t pos = 0;
    while (pos < size) {
        uint32_t tokens = input.integer(), digits = input.integer(), extra = input.integer();
        check(tokens && tokens <= blockTokens, "Bad block length");
        Reader code{input.take(digits)}, suffix{input.take(extra)};
        uint32_t state = le32(code.take(4).data()), buffer = 0;
        unsigned bits = 0;
        check(state >= lower && state < lower * 256u, "Invalid starting state");
        for (unsigned i = 0; i < tokens; ++i) {
            check(pos < size, "Output overflow");
            auto p = history.predict(out.data(), pos);
            check(populated[p.context], "Missing context");
            unsigned slot = state & (unit - 1);
            unsigned sym = lookup[size_t(p.context) * unit + slot];
            const auto& row = model[p.context];
            state = (row[sym+1] - row[sym]) * (state >> 12) + slot - row[sym];
            while (state < lower) state = (state << 8) | code.byte();
            size_t n = 1;
            if (sym < 256) out[pos] = char(literal(sym, out.data(), pos, p));
            else {
                unsigned route = (sym - 256) / 16, k = (sym - 256) % 16;
                while (bits < k) { buffer |= suffix.byte() << bits; bits += 8; }
                unsigned scale = 1u << k;
                n = 2 + scale + (buffer & (scale - 1));
                buffer >>= k;
                bits -= k;
                size_t from = p.from[route];
                check(from && from < pos && n <= size - pos, "Invalid match");
                if (n <= pos - from) std::memcpy(out.data()+pos, out.data()+from, n);
                else for (size_t j = 0; j < n; ++j) out[pos+j] = out[from+j];
            }
            observe(out.data(), pos, n, sym < 256);
            history.learn(out.data(), pos, n);
            pos += n;
        }
        check(state == lower && code.at == code.data.size() && suffix.at == suffix.data.size() && !buffer,
              "Block did not end exactly");
    }
    check(input.at == input.data.size(), "Trailing bytes");
    return out;
}
Bytes restore(View archive) {
    return restoreUsing(archive, "WCP1", [](unsigned s, const char*, size_t, Prediction) { return s; },
                        [](const char*, size_t, size_t, bool) {});
}

#ifndef WIKI_CODEC_LIBRARY
int main(int argc, char** argv) {
    try {
        check(argc == 4, "Usage: context-codec pack|restore INPUT OUTPUT");
        auto input = readFile(argv[2]);
        Bytes result;
        if (View(argv[1]) == "restore") result = restore(input);
#ifndef DECODE_ONLY
        else if (View(argv[1]) == "pack") result = encode(input);
#endif
        else throw std::runtime_error("Unknown mode");
        std::ofstream out(argv[3], std::ios::binary);
        out.write(result.data(), result.size());
        out.close();
        check(bool(out), "Write failed");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
#endif
