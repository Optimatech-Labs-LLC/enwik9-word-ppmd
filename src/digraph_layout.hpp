// Pair codes live only in spellings and literal text. Word IDs keep their
// original values and widths, including any bytes that resemble pair codes.
namespace digraph {
constexpr std::array<View,7> pairs{"th","sh","ch","ph","wh","ng","ck"};
constexpr std::array<uint8_t,28> markers{
    0,1,2,3,4,5,6,7,8,11,12,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
};
constexpr unsigned escape=31;
struct Alphabet {
    std::array<int8_t,256> inverse{};
    std::array<int8_t,26*26> pairIndex{};
    Alphabet(){
        inverse.fill(-1);pairIndex.fill(-1);
        for(size_t i=0;i<markers.size();++i)inverse[markers[i]]=int8_t(i);
        for(size_t i=0;i<pairs.size();++i)
            pairIndex[(pairs[i][0]-'a')*26+pairs[i][1]-'a']=int8_t(i);
    }
    int code(unsigned a,unsigned b)const{
        unsigned first=(a|32)-'a',second=(b|32)-'a';
        if(first>=26 || second>=26)return -1;
        int index=pairIndex[first*26+second];
        return index<0?-1:index*4+2*(a<'a')+(b<'a');
    }
    void expand(Bytes& out,unsigned code)const{
        View pair=pairs[code/4];out.push_back(char(pair[0]-((code&2)?32:0)));
        out.push_back(char(pair[1]-((code&1)?32:0)));
    }
};
inline const Alphabet alphabet;
#ifdef WIKI_DIGRAPH_FIRST
inline Bytes expandText(View input,size_t expected){
    Bytes out;out.reserve(expected);Reader in{input};
    while(in.at<input.size()){
        unsigned c=in.byte();int code=alphabet.inverse[c];
        if(code>=0)alphabet.expand(out,unsigned(code));
        else if(c==escape){
            unsigned original=in.byte();check(original>=128 && original<160,"Invalid quoted source control");
            original-=128;check(original==escape || alphabet.inverse[original]>=0,"Unknown source control");
            out.push_back(char(original));
        }else out.push_back(char(c));
        check(out.size()<=expected,"Expanded text exceeds source length");
    }
    check(out.size()==expected,"Incomplete pair expansion");return out;
}
#ifndef DECODE_ONLY
inline Bytes compactText(View input){
    std::array<uint64_t,pairs.size()> counts{};uint64_t escapes=0;
    Bytes out;out.reserve(input.size());
    for(size_t i=0;i<input.size();++i){
        unsigned c=uint8_t(input[i]);int code=i+1<input.size()?alphabet.code(c,uint8_t(input[i+1])):-1;
        if(code>=0){out.push_back(char(markers[code]));++counts[code/4];++i;}
        else if(c==escape || alphabet.inverse[c]>=0){out.push_back(char(escape));out.push_back(char(c+128));++escapes;}
        else out.push_back(char(c));
    }
    std::cerr<<"{\"digraph_order\":[\"th\",\"sh\",\"ch\",\"ph\",\"wh\",\"ng\",\"ck\"],\"pairs_before_removing_vowels\":[";
    for(size_t i=0;i<counts.size();++i)std::cerr<<(i?",":"")<<counts[i];
    std::cerr<<"],\"escaped_literal_controls\":"<<escapes<<",\"bytes_after_pair_substitution\":"<<out.size()<<"}\n";
    return out;
}
#endif
#endif
inline size_t tail(unsigned lead) {
    if(lead<208 || lead==groups::spaceFlip)return 0;
    if(lead<240 || lead==groups::escaped)return 1;
    check(lead==240,"Bad digraph word prefix");return 2;
}
inline Bytes expandDictionary(View data){
    Reader r{data};unsigned n=r.integer();check(n<=groups::vocabulary,"Too many digraph spellings");
    Bytes ret;put(ret,n);
    for(unsigned i=0;i<n;++i){
        unsigned size=r.byte();check(size && size<=40,"Invalid compact spelling");View w=r.take(size);
        auto start=ret.size();ret.push_back(0);
        for(uint8_t c:w){
            int code=alphabet.inverse[c];
            if(code>=0)alphabet.expand(ret,unsigned(code));
            else {check(groups::wordKind(c),"Invalid spelling character");ret.push_back(char(c));}
        }
        auto len=ret.size()-start-1;check(len>=2 && len<=40,"Bad expanded spelling");
        ret[start]=char(len);
    }
    check(r.at==data.size(),"Trailing dictionary bytes");return ret;
}
inline Bytes expandFrame(View frame,size_t dictionary,size_t protectedBytes,size_t expected){
    Reader in{frame};Bytes out=expandDictionary(in.take(dictionary));
    check(out.size()<=expected && protectedBytes<=expected-out.size(),"Invalid protected prefix");
    out.reserve(expected);out.append(in.take(protectedBytes));
    while(in.at<frame.size()){
        unsigned c=in.byte();int code=alphabet.inverse[c];
        if(code>=0)alphabet.expand(out,unsigned(code));
        else if(c==escape){
            unsigned original=in.byte();
            check(original==escape || alphabet.inverse[original]>=0,"Invalid digraph escape");
            out.push_back(char(original));
        }else {out.push_back(char(c));out.append(in.take(tail(c)));}
        check(out.size()<=expected,"Digraph frame overflow");
    }
    check(out.size()==expected,"Incomplete digraph frame");return out;
}
#ifndef DECODE_ONLY
using Tally=std::array<uint64_t,pairs.size()>;
struct Encoded {
    Bytes bytes;
    size_t dictionary=0,protectedBytes=0;
    Tally inDictionary{},inLiterals{},storedSpellings{};
    uint64_t escaped=0;
    void report()const{
        auto list=[](const Tally& t){std::cerr<<'[';for(size_t i=0;i<t.size();++i)std::cerr<<(i?",":"")<<t[i];std::cerr<<']';};
        std::cerr<<"{\"digraph_order\":[\"th\",\"sh\",\"ch\",\"ph\",\"wh\",\"ng\",\"ck\"],\"pairs_in_dictionary_word_occurrences\":";
        list(inDictionary);std::cerr<<",\"pairs_in_literal_text\":";list(inLiterals);
        std::cerr<<",\"pairs_in_stored_spellings\":";list(storedSpellings);
        std::cerr<<",\"escaped_literal_controls\":"<<escaped<<",\"digraph_frame_bytes\":"<<bytes.size()
                 <<",\"digraph_dictionary_bytes\":"<<dictionary<<",\"protected_wire_bytes\":"<<protectedBytes<<"}\n";
    }
};
inline unsigned dictionaryId(unsigned code){return (code&~7u)+((5u*code)&7);}
inline Encoded encodeFrame(View frame,size_t directoryEnd){
    Reader in{frame};unsigned n=in.integer();check(n<=groups::vocabulary,"Too many words");
    Encoded out;out.bytes.reserve(frame.size());put(out.bytes,n);
    std::vector<Tally> counts(n);
    std::vector<View> dictionary(n);
    for(unsigned id=0;id<n;++id){
        auto word=in.take(in.byte());check(word.size()>=2 && word.size()<=40,"Bad source spelling");
        dictionary[id]=word;size_t at=out.bytes.size();out.bytes.push_back(0);
        for(size_t j=0;j<word.size();++j){
            int code=j+1<word.size()?alphabet.code(uint8_t(word[j]),uint8_t(word[j+1])):-1;
            if(code<0)out.bytes.push_back(word[j]);
            else {out.bytes.push_back(char(markers[code]));++counts[id][code/4];++out.storedSpellings[code/4];++j;}
        }
        out.bytes[at]=char(out.bytes.size()-at-1);
    }
    out.dictionary=out.bytes.size();size_t start=in.at,pagePosition=0;bool flip=false;unsigned last=0;
    // The synthetic directory is binary. Protect complete wire tokens through
    // its end so its numbers cannot be mistaken for spelling substitutions.
    while(pagePosition<directoryEnd){
        unsigned c=in.byte();
        if(c==groups::spaceFlip){flip=true;continue;}
        if(c<128 || c==groups::escaped){last=c==groups::escaped?in.byte():c;++pagePosition;continue;}
        unsigned id=dictionaryId(groups::readId(in,c));check(id<n,"Invalid prefix word");
        View w=dictionary[id];pagePosition+=w.size()+(groups::usualSpace(last)!=flip);last=uint8_t(w.back());flip=false;
    }
    out.protectedBytes=in.at-start;out.bytes.append(frame.substr(start,out.protectedBytes));
    while(in.at<frame.size()){
        size_t pos=in.at;unsigned c=in.byte();
        if(c<128){
            int code=in.at<frame.size()?alphabet.code(c,uint8_t(frame[in.at])):-1;
            if(code>=0){out.bytes.push_back(char(markers[code]));++out.inLiterals[code/4];++in.at;continue;}
            if(c==escape || alphabet.inverse[c]>=0){out.bytes.push_back(char(escape));++out.escaped;}
            out.bytes.push_back(char(c));continue;
        }
        if(c<=240){
            unsigned id=dictionaryId(groups::readId(in,c));check(id<n,"Invalid body word");
            for(size_t k=0;k<pairs.size();++k)out.inDictionary[k]+=counts[id][k];
        }else in.take(tail(c));
        out.bytes.append(frame.substr(pos,in.at-pos));
    }
    return out;
}
#endif
}
