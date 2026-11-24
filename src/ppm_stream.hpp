// Public-domain PPMd variant I from the 7-Zip C SDK is linked into both tools.
#include "Ppmd8.h"
#include <cstdlib>

namespace ppm {
#ifdef WIKI_WIDE_PPM
constexpr unsigned order=16;
inline size_t memory=size_t(1536)<<20;
#elif defined(WIKI_LARGE_WORD_PPM)
constexpr unsigned order = 16, memory = 1536u << 20;
#else
constexpr unsigned order = 10, memory = 512u << 20;
#endif
struct Model {
    ISzAlloc allocator = {
        [](ISzAllocPtr, size_t n) { return std::malloc(n); },
        [](ISzAllocPtr, void *p) { std::free(p); }
    };
    CPpmd8 state{};
    Model() {
        Ppmd8_Construct(&state);
        check(Ppmd8_Alloc(&state, memory, &allocator), "Cannot allocate PPMd model");
        Ppmd8_Init(&state, order, PPMD8_RESTORE_METHOD_RESTART);
    }
    ~Model() { Ppmd8_Free(&state, &allocator); }
};
struct Input : IByteIn {
    View bytes;
    size_t at = 0;
    bool exhausted = false;
    explicit Input(View v) : bytes(v) {
        Read = [](IByteInPtr ptr) -> Byte {
            auto& in = *static_cast<Input*>(const_cast<IByteIn*>(ptr));
            if (in.at < in.bytes.size()) return uint8_t(in.bytes[in.at++]);
            in.exhausted = true;
            return 0;
        };
    }
};
Bytes unpack(View stream, size_t length) {
    check(length <= limit && stream.size() >= 4, "Wrong PPMd length");
    Model m;
    Input in(stream);
    m.state.Stream.In = &in;
    check(Ppmd8_Init_RangeDec(&m.state), "Bad range stream");
    Bytes out(length, '\0');
    for (char& c : out) {
        int decoded = Ppmd8_DecodeSymbol(&m.state);
        check(decoded >= 0, "Premature PPMd end");
        c = char(decoded);
    }
    check(Ppmd8_DecodeSymbol(&m.state) == PPMD8_SYM_END &&
          Ppmd8_RangeDec_IsFinishedOK(&m.state) && !in.exhausted && in.at == in.bytes.size(),
          "Invalid PPMd ending");
    return out;
}
#ifndef DECODE_ONLY
struct Output : IByteOut {
    Bytes bytes;
    Output() {
        Write = [](IByteOutPtr ptr, Byte c) {
            static_cast<Output*>(const_cast<IByteOut*>(ptr))->bytes.push_back(char(c));
        };
    }
};
Bytes pack(View src) {
    Model m;
    Output out;
    m.state.Stream.Out = &out;
    Ppmd8_Init_RangeEnc(&m.state);
    for (uint8_t c : src) Ppmd8_EncodeSymbol(&m.state, c);
    Ppmd8_EncodeSymbol(&m.state, PPMD8_SYM_END);
    Ppmd8_Flush_RangeEnc(&m.state);
    return std::move(out.bytes);
}
#endif
}
