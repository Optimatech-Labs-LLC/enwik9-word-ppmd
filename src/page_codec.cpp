// Reorder Wikipedia pages and optionally copy runs from the preceding page.
// Build with -DDECODE_ONLY to produce the self-contained restore executable.
#ifndef NO_ZSTD
#include <zstd.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wiki_pages {
using Bytes = std::string;
using View = std::string_view;
using std::size_t;
constexpr size_t maxBytes = size_t{2} << 30;
constexpr size_t minCopy = 32;

void require(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}

Bytes readFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot open input");
    auto length = file.tellg();
    require(length >= 0 && uint64_t(length) <= maxBytes, "Unsupported input length");
    Bytes bytes(size_t(length), '\0');
    file.seekg(0);
    require(bool(file.read(bytes.data(), bytes.size())), "Incomplete input read");
    return bytes;
}

void writeFile(const char* path, View bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    file.close();
    require(bool(file), "Incomplete output write");
}

struct Reader {
    View bytes;
    size_t at = 0;
    View take(size_t n) {
        require(n <= bytes.size() - at, "Truncated data");
        auto s = bytes.substr(at, n);
        at += n;
        return s;
    }
    size_t integer() {
        uint64_t value = 0;
        for (int shift = 0; shift <= 28; shift += 7) {
            auto byte = uint8_t(take(1)[0]);
            value |= uint64_t(byte & 127) << shift;
            if (byte < 128) {
                require(value <= maxBytes, "Integer out of range");
                return size_t(value);
            }
        }
        throw std::runtime_error("Invalid integer");
    }
};

Bytes restoreFrame(View frame) {
    Reader input{frame};
    require(input.take(4) == "WPG1", "Invalid page archive");
    int mode = uint8_t(input.take(1)[0]);
    require(mode >= 1 && mode <= 3, "Unknown page codec");
    size_t count = input.integer();
    size_t instructionSize = input.integer();
    require(count && count <= (input.bytes.size() - input.at) / 2, "Invalid page count");
    std::vector<size_t> order(count), sizes(count), offsets(count + 1);
    std::vector<size_t> leading(count), body(count);
    std::vector<bool> seen(count);
    size_t total = 0;
    for (auto& id : order) {
        id = input.integer();
        require(id < count && !seen[id], "Invalid page order");
        seen[id] = true;
        sizes[id] = input.integer();
        if (mode == 3) {
            leading[id] = input.integer();
            body[id] = input.integer();
            require(leading[id] <= sizes[id] && body[id] <= sizes[id] - leading[id], "Invalid text bounds");
        }
        require(sizes[id] <= maxBytes - total, "Oversized output");
        total += sizes[id];
    }
    std::partial_sum(sizes.begin(), sizes.end(), offsets.begin() + 1);
    Reader instructions{input.take(instructionSize)};
    Bytes output(total, '\0');
    if (mode == 3) {
        auto place = [&](size_t offset, size_t n) {
            auto s = input.take(n);
            if (n) std::memcpy(output.data() + offset, s.data(), n);
        };
        for (size_t id = 0; id < count; ++id) place(offsets[id], leading[id]);
        for (size_t id = 0; id < count; ++id) {
            size_t end = leading[id] + body[id];
            place(offsets[id] + end, sizes[id] - end);
        }
        for (size_t id : order) place(offsets[id] + leading[id], body[id]);
        require(!instructionSize && input.at == input.bytes.size(), "Trailing field data");
        return output;
    }
    View previous;
    for (size_t id : order) {
        char* dest = output.data() + offsets[id];
        size_t used = 0;
        auto append = [&](View s) {
            require(s.size() <= sizes[id] - used, "Invalid page length");
            if (!s.empty()) std::memcpy(dest + used, s.data(), s.size());
            used += s.size();
        };
        size_t copies = mode == 2 ? instructions.integer() : 0;
        for (size_t i = 0; i < copies; ++i) {
            append(input.take(instructions.integer()));
            size_t from = instructions.integer();
            size_t n = minCopy + instructions.integer();
            require(from <= previous.size() && n <= previous.size() - from, "Invalid copy reference");
            append(previous.substr(from, n));
        }
        append(input.take(sizes[id] - used));
        previous = View(dest, sizes[id]);
    }
    require(instructions.at == instructions.bytes.size() && input.at == input.bytes.size(),
            "Trailing data");
    return output;
}

#ifndef NO_ZSTD
Bytes restore(View compressed) {
    auto length = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    require(length <= maxBytes, "Invalid or oversized Zstandard frame");
    Bytes frame(size_t(length), '\0');
    require(ZSTD_decompress(frame.data(), frame.size(), compressed.data(), compressed.size()) == length,
            "Invalid Zstandard payload");
    return restoreFrame(frame);
}
#endif

#ifndef DECODE_ONLY
void put(Bytes& out, size_t n) {
    while (n >= 128) {
        out.push_back(char((n & 127) | 128));
        n >>= 7;
    }
    out.push_back(char(n));
}

Bytes normalized(View s) {
    Bytes out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '_') c = ' ';
    }
    return out;
}

View field(View s, View open, View close) {
    size_t begin = s.find(open);
    if (begin == View::npos) return {};
    begin += open.size();
    size_t end = s.find(close, begin);
    return end == View::npos ? View{} : s.substr(begin, end - begin);
}

struct Page {
    View data;
    Bytes key, title;
    std::vector<Bytes> labels;
};

void collectLabels(Page& page, View prefix, View endChars, char type, int limit) {
    size_t pos = 0;
    for (int i = 0; i < limit; ++i) {
        pos = page.data.find(prefix, pos);
        if (pos == View::npos) return;
        pos += prefix.size();
        View name = page.data.substr(pos, 81);
        size_t end = name.find_first_of(endChars);
        if (end >= 2 && end <= 80)
            page.labels.push_back(type + normalized(name.substr(0, end)));
    }
}

std::vector<size_t> pageOrder(std::vector<Page>& pages) {
    std::unordered_map<Bytes, size_t> frequency;
    for (size_t i = 1; i < pages.size(); ++i) {
        auto& p = pages[i];
        p.title = normalized(field(p.data, "<title>", "</title>"));
        size_t body = p.data.find("<text ");
        if (body != View::npos) body = p.data.find('>', body);
        if (body != View::npos) {
            View start = p.data.substr(body + 1, 220);
            if (normalized(start.substr(0, 9)) == "#redirect") {
                p.key = "R";
                p.title = normalized(field(start, "[[", "]]")) + '\n' + p.title;
                continue;
            }
        }
        collectLabels(p, "[[Category:", "|]\r\n", 'C', 10);
        collectLabels(p, "{{", "|}\r\n", 'T', 6);
        std::sort(p.labels.begin(), p.labels.end());
        p.labels.erase(std::unique(p.labels.begin(), p.labels.end()), p.labels.end());
        for (const auto& s : p.labels) ++frequency[s];
    }
    for (size_t i = 1; i < pages.size(); ++i) {
        auto& p = pages[i];
        if (p.key == "R") continue;
        p.key = "~";
        double best = std::numeric_limits<double>::infinity();
        for (const auto& s : p.labels) {
            size_t f = frequency.at(s);
            if (f < 4) continue;
            double distance = std::abs(std::log2(double(f) / 128));
            if (distance < best) {
                best = distance;
                p.key = s;
            }
        }
    }
    std::vector<size_t> order(pages.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin() + 1, order.end(), [&](size_t a, size_t b) {
        if (pages[a].key != pages[b].key) return pages[a].key < pages[b].key;
        return pages[a].title < pages[b].title;
    });
    return order;
}

struct Copy { size_t gap, from, length; };
struct Slot { uint32_t page = 0, pos = 0; };

uint16_t slot(const char* p) {
    uint64_t word;
    std::memcpy(&word, p, 8);
    word ^= word >> 33;
    return uint16_t((word * UINT64_C(11400714819323198485)) >> 48);
}

void encodeDifferences(View prev, View current, uint32_t epoch, std::vector<Slot>& index,
                       Bytes& instructions, Bytes& literals, size_t& copied) {
    for (size_t i = 0; i + 8 <= prev.size(); i += 4)
        index[slot(prev.data() + i)] = {epoch, uint32_t(i)};
    size_t i = 0, literalStart = 0;
    std::vector<Copy> matches;
    while (i + minCopy <= current.size()) {
        const auto hit = index[slot(current.data() + i)];
        if (hit.page != epoch || prev.size() - hit.pos < minCopy ||
            std::memcmp(prev.data() + hit.pos, current.data() + i, minCopy) != 0) {
            ++i;
            continue;
        }
        size_t n = minCopy, max = std::min(prev.size() - hit.pos, current.size() - i);
        while (n < max && prev[hit.pos + n] == current[i + n]) ++n;
        matches.push_back({i - literalStart, hit.pos, n});
        literals.append(current.substr(literalStart, i - literalStart));
        copied += n;
        i += n;
        literalStart = i;
    }
    literals.append(current.substr(literalStart));
    put(instructions, matches.size());
    for (const auto& m : matches) {
        put(instructions, m.gap);
        put(instructions, m.from);
        put(instructions, m.length - minCopy);
    }
}

// Preserve the tag spelling, indentation, and the incomplete last article.
std::pair<size_t, size_t> textBounds(View page) {
    size_t open = page.find("<text ");
    if (open == View::npos) return {page.size(), 0};
    open = page.find('>', open);
    if (open == View::npos || page[open - 1] == '/') return {page.size(), 0};
    size_t begin = open + 1, end = page.find("</text>", begin);
    return {begin, (end == View::npos ? page.size() : end) - begin};
}

Bytes transform(View raw, bool copy, bool sorted, bool fields) {
    constexpr View marker = "  <page>\n";
    std::vector<Page> pages;
    size_t begin = 0, end = raw.find(marker);
    pages.push_back({raw.substr(0, end), {}, {}, {}});
    while (end != View::npos) {
        begin = end;
        end = raw.find(marker, begin + marker.size());
        pages.push_back({raw.substr(begin, end == View::npos ? end : end - begin), {}, {}, {}});
    }
    std::vector<size_t> order(pages.size());
    if (sorted) order = pageOrder(pages);
    else std::iota(order.begin(), order.end(), 0);
    Bytes directory, instructions, literals;
    literals.reserve(raw.size());
    std::vector<std::pair<size_t, size_t>> bounds;
    if (fields) {
        for (const auto& page : pages) {
            auto [begin, length] = textBounds(page.data);
            bounds.emplace_back(begin, length);
            literals.append(page.data.substr(0, begin));
        }
        for (size_t id = 0; id < pages.size(); ++id) {
            auto [begin, length] = bounds[id];
            literals.append(pages[id].data.substr(begin + length));
        }
    }
    std::vector<Slot> index(65536);
    View previous;
    size_t copied = 0;
    uint32_t epoch = 0;
    for (auto id : order) {
        View p = pages[id].data;
        put(directory, id);
        put(directory, p.size());
        if (fields) {
            auto [begin, length] = bounds[id];
            put(directory, begin);
            put(directory, length);
            literals.append(p.substr(begin, length));
        } else if (copy) encodeDifferences(previous, p, ++epoch, index, instructions, literals, copied);
        else literals.append(p);
        previous = p;
    }
    Bytes out("WPG1");
    out.push_back(fields ? 3 : copy ? 2 : 1);
    put(out, order.size());
    put(out, instructions.size());
    out.reserve(out.size() + directory.size() + instructions.size() + literals.size());
    out.append(directory).append(instructions).append(literals);
    std::cerr << "Pages: " << pages.size() - 1 << ", bytes copied: " << copied
              << ", directory: " << directory.size() << ", instructions: " << instructions.size() << '\n';
    return out;
}

#ifndef NO_ZSTD
Bytes compress(View source) {
    Bytes compressed(ZSTD_compressBound(source.size()), '\0');
    size_t n = ZSTD_compress(compressed.data(), compressed.size(), source.data(), source.size(), 9);
    require(!ZSTD_isError(n), ZSTD_getErrorName(n));
    compressed.resize(n);
    return compressed;
}
#endif
#endif
} // namespace wiki_pages

#ifndef WIKI_PAGE_LIBRARY
int main(int argc, char** argv) {
    using namespace wiki_pages;
    try {
        require(argc == 4, "Usage: page-codec MODE INPUT OUTPUT");
        View mode(argv[1]);
        auto source = readFile(argv[2]);
        if (mode == "unframe") writeFile(argv[3], restoreFrame(source));
#ifndef NO_ZSTD
        else if (mode == "restore") writeFile(argv[3], restore(source));
#endif
#ifndef DECODE_ONLY
        else if (mode == "prepare-fields") writeFile(argv[3], transform(source, false, true, true));
#ifndef NO_ZSTD
        else if (mode == "raw") writeFile(argv[3], compress(source));
        else if (mode == "group" || mode == "group-copy" || mode == "copy" ||
                 mode == "fields" || mode == "group-fields") {
            auto frame = transform(source, mode == "copy" || mode == "group-copy",
                                   mode == "group" || mode == "group-copy" || mode == "group-fields",
                                   mode == "fields" || mode == "group-fields");
            source.clear();
            source.shrink_to_fit();
            writeFile(argv[3], compress(frame));
        }
#endif
#endif
        else throw std::runtime_error("Unknown codec mode");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#endif
