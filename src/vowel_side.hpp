// The receiver has the consonant stream before reading any vowel choices.
namespace vowels {
using namespace window;
constexpr View letters="aeiouAEIOU";
struct Alphabet {
    std::array<uint8_t,256> digit{};
    Alphabet(){for(unsigned i=0;i<letters.size();++i)digit[uint8_t(letters[i])]=uint8_t(i+1);}
};
inline const Alphabet abc;
bool consonant(unsigned c){return c<256 && groups::letter(c) && !abc.digit[c];}
uint64_t wordHash(View s){
    uint64_t h=UINT64_C(14695981039346656037);
    for(uint8_t c:s)h=(h^c)*UINT64_C(1099511628211);
    return h^(h>>33);
}
uint32_t key32(uint64_t a){return groups::hash(uint32_t(a)^uint32_t(a>>32));}
struct GapRow {
    std::array<uint16_t,11> counts{};unsigned total=0;
    void observe(unsigned d){
        if(total>=2048){total=0;for(auto& c:counts){c/=2;total+=c;}}
        ++counts[d];++total;
    }
};
struct GapModel {
    struct Tagged {uint64_t tag=0;GapRow row;};
    std::array<GapRow,4*11> general{};
    std::vector<GapRow> local=std::vector<GapRow>(1<<16);
    std::vector<Tagged> word=std::vector<Tagged>(1<<18);
    template<class Range> unsigned next(Range& r,unsigned actual,unsigned left,unsigned right,
                                        unsigned previous,unsigned position,unsigned kind,uint64_t skeleton){
        auto& a=general[kind*11+previous];
        uint32_t neighbors=(left*257+right)*11+previous;
        auto& b=local[groups::hash(neighbors+kind*0x9e3779b9u)&(local.size()-1)];
        uint64_t key=skeleton^(uint64_t(position+1)*0x9e3779b97f4a7c15ULL)^previous;
        auto& keyed=word[key32(key)&(word.size()-1)];
        if(keyed.tag!=key){keyed.tag=key;keyed.row={};}auto& c=keyed.row;
        unsigned ga=std::max(1u,48000u/(a.total+11));
        unsigned gb=160000u/(b.total+20),gc=80000u/(c.total+8);
        unsigned total=ga*(a.total+11)+gb*b.total+gc*c.total;
        auto target=r.threshold(total);unsigned before=0;
        for(unsigned i=0;i<11;++i){
            unsigned mass=(a.counts[i]+1)*ga+b.counts[i]*gb+c.counts[i]*gc;
            if(Range::reading?target<before+mass:i==actual){
                r.commit(before,mass);a.observe(i);b.observe(i);c.observe(i);return i;
            }
            before+=mass;
        }
        throw std::runtime_error("Invalid vowel choice");
    }
};
struct SpellRow {
    std::array<Bytes,8> forms{};
    std::array<uint64_t,8> hashes{};
    std::array<uint16_t,8> freq{};
    uint64_t tag=0;
    unsigned used=0,total=0,escapes=0;
    void observe(View s,unsigned index){
        if(total>=2048){total=0;escapes/=2;for(unsigned i=0;i<used;++i){freq[i]=(freq[i]+1)/2;total+=freq[i];}}
        if(index==used){
            ++escapes;
            if(used<forms.size()){index=used++;forms[index]=s;freq[index]=0;}
            else{
                index=unsigned(std::min_element(freq.begin(),freq.end())-freq.begin());
                if(freq[index]>2){--freq[index];--total;return;}
                total-=freq[index];forms[index]=s;freq[index]=0;
            }
            hashes[index]=wordHash(s);
        }
        ++freq[index];++total;
    }
};
struct SideModel {
    struct Partner {uint64_t tag=0,spelling=0;};
    std::vector<SpellRow> variants=std::vector<SpellRow>(1<<18);
    std::vector<Partner> partners=std::vector<Partner>(1<<19);
    uint64_t previousWord=0,lookups=0,hits=0,removed=0;
    GapModel gaps;
    template<class Range> Bytes fallback(Range& r,View skeleton,View actual,unsigned before,unsigned after,
                                          uint64_t key,size_t maximum){
        Bytes result;size_t j=0;
        for(size_t i=0;i<=skeleton.size();++i){
            unsigned left=i?uint8_t(skeleton[i-1]):before,right=i<skeleton.size()?uint8_t(skeleton[i]):after;
            unsigned kind=skeleton.empty()?0:!i?1:i==skeleton.size()?2:3,prev=0;
            for(;;){
                unsigned actualDigit=0;
                if constexpr(!Range::reading)if(j<actual.size())actualDigit=abc.digit[uint8_t(actual[j])];
                unsigned d=gaps.next(r,actualDigit,left,right,prev,unsigned(i),kind,key);
                if(!d)break;
                check(result.size()<maximum,"Vowel run exceeds output size");
                result.push_back(letters[d-1]);prev=d;++j;
            }
            if(i<skeleton.size()){
                if constexpr(!Range::reading)check(j<actual.size() && actual[j]==skeleton[i],"Consonant mismatch");
                check(result.size()<maximum,"Word exceeds output size");result.push_back(skeleton[i]);++j;
            }
        }
        if constexpr(!Range::reading)check(j==actual.size(),"Unconsumed vowel run");
        return result;
    }
    template<class Range> Bytes spelling(Range& r,View skeleton,View actual,unsigned left,unsigned right,size_t maximum){
        uint64_t key=skeleton.empty()?UINT64_C(0x913feaa800000000)+(left*257+right):wordHash(skeleton);
        // Very long one-off strings use the bounded gap model directly.
        if(skeleton.size()>80)return fallback(r,skeleton,actual,left,right,key,maximum);
        auto& row=variants[key32(key)&(variants.size()-1)];
        if(row.tag!=key){row.tag=key;row.used=row.total=row.escapes=0;}
        uint64_t pairKey=key^(previousWord*0x9e3779b97f4a7c15ULL);
        auto& partner=partners[key32(pairKey)&(partners.size()-1)];
        unsigned selected=row.used;
        if(row.used){
            std::array<unsigned,8> weights{};unsigned total=0;
            for(unsigned i=0;i<row.used;++i){
                unsigned w=8*row.freq[i];
                if(partner.tag==pairKey && partner.spelling==row.hashes[i])
                    w+=std::min(700u,12u+2*row.total);
                weights[i]=w;total+=w;
            }
            unsigned escape=2+row.used+std::min(512u,2*row.escapes);
            auto target=r.threshold(total+escape);unsigned from=0;
            for(unsigned i=0;i<row.used;++i){
                if(Range::reading?target<from+weights[i]:actual==row.forms[i]){
                    r.commit(from,weights[i]);selected=i;break;
                }
                from+=weights[i];
            }
            if(selected==row.used)r.commit(total,escape);
        }
        ++lookups;hits+=selected<row.used;
        Bytes result=selected<row.used?row.forms[selected]:fallback(r,skeleton,actual,left,right,key,maximum);
        check(result.size()<=maximum && result.size()>=skeleton.size(),"Invalid decoded word length");
        if constexpr(!Range::reading)check(result==actual,"Wrong spelling selected");
        removed+=result.size()-skeleton.size();
        partner={pairKey,wordHash(result)};
        if(result.size()<=128)row.observe(result,selected);
        if(!result.empty())previousWord=partner.spelling;
        return result;
    }
};
template<class Range> Bytes gaps(Range& range,View stripped,View original,size_t expected,SideModel& model){
    Bytes out;if constexpr(Range::reading)out.reserve(expected);
    size_t pos=0,raw=0;
    auto word=[&](size_t from,size_t end){
        size_t stop=raw;
        if constexpr(!Range::reading)while(stop<original.size() && groups::letter(uint8_t(original[stop])))++stop;
        View actual=Range::reading?View{}:original.substr(raw,stop-raw);
        unsigned left=from?uint8_t(stripped[from-1]):256,right=end<stripped.size()?uint8_t(stripped[end]):256;
        auto result=model.spelling(range,stripped.substr(from,end-from),actual,left,right,expected-raw);
        if constexpr(Range::reading)out+=result;
        raw+=result.size();
    };
    for(;;){
        if(pos<stripped.size() && consonant(uint8_t(stripped[pos]))){
            auto start=pos++;while(pos<stripped.size() && consonant(uint8_t(stripped[pos])))++pos;
            word(start,pos);continue;
        }
        if(!pos || !consonant(uint8_t(stripped[pos-1])))word(pos,pos);
        if(pos==stripped.size())break;
        auto c=uint8_t(stripped[pos++]);check(!abc.digit[c] && raw<expected,"Invalid skeleton");
        if constexpr(Range::reading)out.push_back(char(c));
        else check(raw<original.size() && uint8_t(original[raw])==c,"Stripped input lost a separator");
        ++raw;
    }
    check(raw==expected,"Vowel restore length mismatch");return out;
}
size_t directory(View pages){
    wiki_pages::Reader r{pages};check(r.take(4)=="WPG1" && r.take(1)[0]==3,"Expected split page frame");
    size_t n=r.integer();check(r.integer()==0 && n<=pages.size()/4,"Invalid directory");
    for(size_t i=0;i<n;++i)for(unsigned j=0;j<4;++j)r.integer();return r.at;
}
}
