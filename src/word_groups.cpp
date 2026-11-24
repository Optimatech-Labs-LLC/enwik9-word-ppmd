// Reversible word IDs, with optional low bits predicted inside groups of eight.
// PPMd is an established backend. Its raw control and the transformed modes use
// the same order, memory, reset policy and executable.
#define WIKI_CODEC_LIBRARY
#ifndef WIKI_CONTEXT_READY
#include "context_codec.cpp"
#endif
#ifndef WIKI_GROUPS_TRANSFORM_ONLY
#include "ppm_stream.hpp"
#endif
#include <unordered_map>
#ifdef WIKI_RELATED_DICT
#include <cmath>
#include <numeric>
#endif

namespace groups {
#ifdef WIKI_GRAMMAR_CLASSES
inline Bytes usageClasses;
#endif
constexpr unsigned one = 80, two = 8192, vocabulary = 65536;
constexpr unsigned spaceFlip = 248, pairRepeat = 249, singleRepeat = 250, escaped = 255;
enum class Mode : unsigned { raw, ids, octets, phrases, multiplyOnly };
bool letter(unsigned c) {
#ifdef WIKI_DIGRAPH_FIRST
    // All 28 spelling markers are lexical in the pair-before-vowels format.
    if(c<31 && c!=9 && c!=10 && c!=13)return true;
#endif
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool usualSpace(unsigned c) {
    return letter(c) || (c >= '0' && c <= '9') || c == ',' || c == '.' || c == ')' ||
           c == ']' || c == ';' || c == ':' || c == '!' || c == '?';
}
unsigned wordKind(unsigned c) {
#ifdef WIKI_NUMERIC_UTF
    if(c>='0' && c<='9')return 2;
#endif
    return letter(c)?1:0;
}
uint32_t hash(uint32_t a) {
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; return a;
}
struct Tracker {
    uint32_t prev = 0, prev2 = 0;
    unsigned last = 0;
    std::vector<uint8_t> lastInGroup = std::vector<uint8_t>(vocabulary / 8);
    struct Next { uint32_t key = 0, word = 0; };
    std::vector<Next> afterPair, afterOne;
    struct Slot { uint32_t key = 0; uint8_t remainder = 0; };
    std::vector<Slot> afterGroup;
    explicit Tracker(Mode m) {
        if (m == Mode::octets) afterGroup.resize(1<<19);
        if (m == Mode::phrases) { afterPair.resize(1<<19); afterOne.resize(vocabulary+1); }
    }
    uint32_t groupKey(unsigned q) const { return hash(q + prev*0x9e3779b9u); }
    unsigned predicted(unsigned q) const {
        auto k = groupKey(q);
        const auto& slot = afterGroup[k & (afterGroup.size()-1)];
        return slot.key == k ? slot.remainder : lastInGroup[q];
    }
    unsigned recode(unsigned id, Mode mode, bool decoding) const {
        if (mode != Mode::octets && mode != Mode::multiplyOnly) return id;
        unsigned q = id / 8, r = id % 8;
        unsigned guess = mode == Mode::octets ? predicted(q) : 0;
        // Five is its own inverse modulo eight. Both halves travel in the ID.
        unsigned low = decoding ? guess + 5*r : 5*(r - guess);
        return q*8 + (low & 7);
    }
    uint32_t pairKey() const { return hash(prev ^ (prev2*0x9e3779b9u)); }
    uint32_t next(bool pair) const {
        if (!prev) return 0;
        if (!pair) return afterOne[prev].word;
        auto k=pairKey(); const auto& entry=afterPair[k & (afterPair.size()-1)];
        return prev2 && entry.key==k ? entry.word : 0;
    }
    void word(unsigned id, unsigned end, Mode mode) {
        if (mode == Mode::octets) {
            auto q = id / 8, k = groupKey(q);
            lastInGroup[q] = uint8_t(id % 8);
            afterGroup[k & (afterGroup.size()-1)] = {k,uint8_t(id % 8)};
        }
        if (mode == Mode::phrases && prev) {
            afterOne[prev].word = id+1;
            if (prev2) { auto k=pairKey(); afterPair[k & (afterPair.size()-1)]={k,id+1}; }
        }
        prev2=prev; prev=id+1; last=end;
    }
    void literal(unsigned c) {
        if (letter(c) || c=='\n') { prev=0; prev2=0; }
        last=c;
    }
};
void writeId(Bytes& out, unsigned id) {
    if (id < one) { out.push_back(char(128+id)); return; }
    id -= one;
    if (id < two) { out.push_back(char(208+(id>>8))); out.push_back(char(id)); return; }
    id -= two; out.push_back(char(240+(id>>16))); out.push_back(char(id>>8)); out.push_back(char(id));
}
unsigned readId(Reader& in, unsigned lead) {
    check(lead >= 128 && lead <= 240, "Invalid word ID");
    if (lead<208) return lead-128;
    if (lead<240) return one+256*(lead-208)+in.byte();
    unsigned high=in.byte(); return one+two+256*high+in.byte();
}
#ifndef DECODE_ONLY
using Index = std::unordered_map<View,uint32_t>;
struct Dictionary { std::vector<View> words; Index ids; };
#ifdef WIKI_RELATED_DICT
#include "related_dictionary.hpp"
#endif
Dictionary build(View src, unsigned cap) {
    Index freq;
#ifdef WIKI_NUMERIC_UTF
    Index digits;
#endif
    for (size_t i=0;i<src.size();) {
#ifdef WIKI_NUMERIC_UTF
        if(wordKind(uint8_t(src[i]))==2){
            size_t start=i++;while(i<src.size() && wordKind(uint8_t(src[i]))==2)++i;
            if(i-start>=2 && i-start<=12)++digits[src.substr(start,i-start)];
            continue;
        }
#endif
        if (!letter(uint8_t(src[i]))) { ++i; continue; }
        auto start=i++; while(i<src.size() && letter(uint8_t(src[i]))) ++i;
#ifdef WIKI_FOLDED_VOCAB
        // The synthetic page directory stays binary and is never case folded.
        if(std::any_of(src.begin()+start,src.begin()+i,[](char c){return c>='A'&&c<='Z';}))continue;
#endif
        if (i-start>=2 && i-start<=40) ++freq[src.substr(start,i-start)];
    }
    std::vector<std::pair<View,uint32_t>> common; common.reserve(freq.size());
    for (auto item:freq) if (item.second >= 5) common.push_back(item);
    auto byCount=[](const auto& a,const auto& b){
        if(a.second!=b.second) return a.second>b.second;
        return a.first<b.first;
    };
    check(cap<=vocabulary,"Vocabulary exceeds format limit");
    unsigned numberSlots=0;
#ifdef WIKI_NUMERIC_UTF
    auto year=[](View w){return w.size()==4 && w>="1000" && w<="2099";};
    std::vector<std::pair<View,uint32_t>> years,otherNumbers;
    for(const auto& e:digits)if(e.second>=5)(year(e.first)?years:otherNumbers).push_back(e);
    unsigned limit=std::min(1024u,cap/8);
    auto keep=[&](auto& list,size_t room){
        auto take=std::min(list.size(),room);
        std::partial_sort(list.begin(),list.begin()+take,list.end(),byCount);list.resize(take);
    };
    std::sort(years.begin(),years.end(),byCount);
    if(years.size()>limit/2){
        otherNumbers.insert(otherNumbers.end(),years.begin()+limit/2,years.end());years.resize(limit/2);
    }
    keep(otherNumbers,limit-years.size());numberSlots=unsigned(years.size()+otherNumbers.size());
#endif
    size_t n=std::min<size_t>(cap-numberSlots,common.size());
    std::partial_sort(common.begin(),common.begin()+n,common.end(),byCount); common.resize(n);
    Dictionary d; d.words.reserve(n); d.ids.reserve(n);
    for (const auto& item:common) d.words.push_back(item.first);
#ifdef WIKI_RELATED_DICT
    orderByUsage(src,d);
#else
    // Put related spellings near each other without changing the ID length tier.
    for (size_t base=one;base<n;) {
        size_t end=std::min(std::min(n,base+2048),base<one+two?size_t(one+two):n);
        std::sort(d.words.begin()+base,d.words.begin()+end); base=end;
    }
#endif
#ifdef WIKI_NUMERIC_UTF
    std::vector<View> numeric;
    for(const auto& e:years)numeric.push_back(e.first);
    for(const auto& e:otherNumbers)numeric.push_back(e.first);
    d.words.insert(d.words.begin()+std::min<size_t>(2128,n),numeric.begin(),numeric.end());
    unsigned yearEntries=0;for(auto w:numeric)yearEntries+=year(w);
    std::cerr<<"{\"numeric_entries\":"<<numberSlots<<",\"four_digit_year_entries\":"<<yearEntries<<"}\n";
#endif
    for (size_t i=0;i<d.words.size();++i) d.ids.emplace(d.words[i],uint32_t(i));
    return d;
}
Bytes transformWithDictionary(View src, Mode mode, const Dictionary& d) {
    Bytes out; out.reserve(src.size());
    put(out,uint32_t(d.words.size()));
    for(auto w:d.words) { out.push_back(char(w.size())); out.append(w); }
    auto dictBytes=out.size();
    Tracker t(mode); uint64_t tokens=0,matches=0,flips=0,lowZero=0;
    auto literal=[&](unsigned c) {
        if(c>=128) out.push_back(char(escaped));
        out.push_back(char(c)); t.literal(c);
    };
    for(size_t i=0;i<src.size();) {
        auto c=uint8_t(src[i]);
        bool space=c==' ' && i+1<src.size() && wordKind(uint8_t(src[i+1]));
        if(!space && !wordKind(c)) { literal(c); ++i; continue; }
        size_t start=i+space, end=start+1;
        unsigned kind=wordKind(uint8_t(src[start]));
        while(end<src.size() && wordKind(uint8_t(src[end]))==kind) ++end;
        auto it=d.ids.find(src.substr(start,end-start));
        if(it==d.ids.end()) { while(i<end) literal(uint8_t(src[i++])); continue; }
        auto id=it->second; ++tokens;
        if(space!=usualSpace(t.last)) { out.push_back(char(spaceFlip)); ++flips; }
        if(mode==Mode::phrases && t.next(true)==id+1) { out.push_back(char(pairRepeat)); ++matches; }
        else if(mode==Mode::phrases && t.next(false)==id+1) { out.push_back(char(singleRepeat)); ++matches; }
        else {
            unsigned code=t.recode(id,mode,false);
            lowZero+=(code%8)==0; writeId(out,code);
        }
        t.word(id,uint8_t(src[end-1]),mode); i=end;
    }
    std::cerr<<"{\"vocabulary\":"<<d.words.size()<<",\"dictionary_bytes_before_ppmd\":"<<dictBytes
             <<",\"dictionary_tokens\":"<<tokens<<",\"space_exceptions\":"<<flips
             <<",\"third_prediction_matches\":"<<matches<<",\"low_three_bits_zero\":"<<lowZero<<"}\n";
    return out;
}
Bytes transform(View src, Mode mode, unsigned cap=vocabulary) {
    auto dictionary=build(src,cap);
    return transformWithDictionary(src,mode,dictionary);
}
#endif
Bytes inverse(View data, size_t size, Mode mode) {
    Reader in{data}; unsigned n=in.integer(); check(n<=vocabulary,"Too many words");
    std::vector<View> dict;dict.reserve(n);
    for(unsigned i=0;i<n;++i) {
        unsigned len=in.byte();check(len>=2 && len<=40,"Bad spelling length");
        dict.push_back(in.take(len));
    }
    Tracker t(mode); Bytes out; out.reserve(size);
    while(in.at<data.size()) {
        unsigned lead=in.byte();
        if(lead<128 || lead==escaped) {
            unsigned c=lead==escaped?in.byte():lead;check(out.size()<size,"Too much output");
            out.push_back(char(c));t.literal(c);continue;
        }
        bool flip=lead==spaceFlip;
        if(flip) lead=in.byte();
        unsigned id;
        if(lead==pairRepeat || lead==singleRepeat) {
            check(mode==Mode::phrases,"Wrong word shortcut");auto guess=t.next(lead==pairRepeat);
            check(guess,"Unavailable word shortcut");id=guess-1;
        } else {
            auto code=readId(in,lead);check(code<vocabulary,"Word code out of range");
            id=t.recode(code,mode,true);
        }
        check(id<n,"Unknown dictionary word");
        bool space=usualSpace(t.last)!=flip; auto word=dict[id];
        check(word.size()+space<=size-out.size(),"Word extends beyond output");
        if(space) out.push_back(' ');
        out.append(word); t.word(id,uint8_t(word.back()),mode);
    }
    check(out.size()==size,"Incomplete text");return out;
}
constexpr std::array<View,5> names{"raw","ids","octets","phrases","multiply"};
}

#ifndef WIKI_GROUP_LIBRARY
int main(int argc,char** argv) {
    try {
        check(argc>=4,"Usage: word-groups pack MODE INPUT ARCHIVE | restore ARCHIVE OUTPUT");
        Bytes out; const char* target=nullptr;
        if(argc==4 && View(argv[1])=="restore") {
            auto archive=readFile(argv[2]); Reader in{archive};
            check(in.take(4)=="WGP1","Bad word group archive");
            auto mode=in.byte();check(mode<groups::names.size(),"Bad transform");
            auto size=in.integer(), framed=in.integer();check(size<=limit,"Output too large");
            auto data=ppm::unpack(in.data.substr(in.at),framed);
            if(mode) out=groups::inverse(data,size,groups::Mode(mode));
            else { check(size==data.size(),"Wrong raw size");out=std::move(data); }
            target=argv[3];
        }
#ifndef DECODE_ONLY
        else if(argc==5 && View(argv[1])=="pack") {
            auto mode=std::find(groups::names.begin(),groups::names.end(),View(argv[2]));
            check(mode!=groups::names.end(),"Unknown transform");
            auto which=groups::Mode(mode-groups::names.begin());auto src=readFile(argv[3]);
            out="WGP1";out.push_back(char(which));put(out,uint32_t(src.size()));
            if(which!=groups::Mode::raw) src=groups::transform(src,which);
            else std::cerr<<"{}\n";
            put(out,uint32_t(src.size()));out+=ppm::pack(src);target=argv[4];
        }
#endif
        else throw std::runtime_error("Invalid arguments");
        if(View(target)=="-") { std::cout.write(out.data(),out.size());std::cout.flush();check(bool(std::cout),"Output failed"); }
        else { std::ofstream file(target,std::ios::binary); file.write(out.data(),out.size());file.close();check(bool(file),"Output failed"); }
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
