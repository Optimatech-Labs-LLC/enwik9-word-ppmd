// Causal experts for the mixed word/byte model. The original model covers every outcome.
namespace token_specialists {
using namespace phrase;
inline size_t protectedWire=0;

template<unsigned N> struct Counts {
    std::array<uint16_t,N> count{};
    unsigned total=0;
    void add(unsigned symbol){
        if(total>=2048){total=0;for(auto& v:count){v=(v+1)/2;total+=v;}}
        ++count[symbol];++total;
    }
};
template<class Row> class Tagged {
    struct Slot {uint32_t tag=0;bool used=false;Row row;};
    std::vector<Slot> slots;
public:
    explicit Tagged(unsigned size):slots(size){}
    const Row* get(uint32_t key)const{
        const auto& s=slots[key&(slots.size()-1)];return s.used && s.tag==key?&s.row:nullptr;
    }
    Row& write(uint32_t key){
        auto& s=slots[key&(slots.size()-1)];
        if(!s.used || s.tag!=key){s.row={};s.used=true;s.tag=key;}return s.row;
    }
};
struct State {
    std::array<int,192> gates{};
    unsigned total=0;int* gate=nullptr;bool active=false;
    uint64_t predicted=0,trained=0;
    State(){gates.fill(48<<12);}
};
template<class Base,class Expert> struct Blend {
    const Base& base;Expert& extra;
    unsigned total,support,strength,factor;
    Blend(const Base& b,Expert& e):base(b),extra(e),total(b.total),support(b.support),strength(b.strength){
        const unsigned share=unsigned(*extra.gate)>>12;
        uint64_t denominator=uint64_t(1024-share)*e.total;
        factor=unsigned((uint64_t(b.total)*share+denominator/2)/denominator);
        total+=factor*e.total;
    }
    unsigned operator()(unsigned c)const{return base(c)+factor*extra.mass(c);}
    void observe(unsigned c){
        unsigned prior=unsigned(*extra.gate)>>12;
        uint64_t a=uint64_t(prior)*extra.mass(c)*base.total;
        uint64_t b=uint64_t(1024-prior)*base(c)*extra.total;
        if(a+b){
            auto post=int((__uint128_t(a)*(1024u<<12))/(a+b));
            auto& gain=*extra.gate;gain=std::clamp(gain+(post-gain)/256,2<<12,720<<12);
        }
    }
};
#ifdef WIKI_MARKUP_CONTEXT
struct Markup:State {
    using Row=Counts<256>;
    Tagged<Row> exact{8192},coarse{2048};
    const Row* detail=nullptr;const Row* general=nullptr;
    unsigned link=0,templ=0,table=0,heading=0,style=0,lastPunct=0,pending=0,quotes=0,atStart=1,lastKind=0;
    uint32_t key=0,shortKey=0;unsigned gateIndex=0;
    bool quoted=false;
    static bool punctuation(unsigned c){return c<128 && (std::ispunct(c) || std::isspace(c));}
    void start(const History192& h,const Features&,const Features::Query&,bool enabled){
        active=enabled;total=0;if(!enabled)return;
        unsigned flags=(link>0)|((templ>0)<<1)|((table>0)<<2)|((heading>0)<<3)|(style<<4);
        unsigned depth=std::min(7u,link+templ+table);
        shortKey=combine(flags*8+depth,lastPunct);
        key=combine(shortKey,atStart*8+lastKind);
        detail=exact.get(key);general=coarse.get(shortKey);
        gateIndex=((flags!=0)*2+(depth>1))*24+std::min(5u,lastKind)*4+std::min(3u,h.retrieval->strength());
    }
    void select(unsigned node){
        total=0;if(!active || node)return;
        total=(general?general->total:0)+4*(detail?detail->total:0);gate=&gates[gateIndex];
        predicted+=total!=0;
    }
    unsigned mass(unsigned c)const{return (general?general->count[c]:0)+4*(detail?detail->count[c]:0);}
    void text(unsigned c){
        if(c=='\n' || c=='\r'){heading=0;atStart=1;style=0;pending=quotes=0;return;}
        if(c=='=' && (atStart||heading)){if(atStart)heading=std::min(6u,heading+1);}
        else if(c!=' ' && c!='\t')atStart=0;
        if(c=='\''){
            ++quotes;
            if(quotes==2)style^=1;
            else if(quotes==3)style^=3;
            else if(quotes==5)style^=1;
        } else quotes=0;
        if(pending=='[' && c=='['){++link;pending=0;}
        else if(pending==']' && c==']'){if(link)--link;pending=0;}
        else if(pending=='{' && c=='{'){++templ;pending=0;}
        else if(pending=='}' && c=='}'){if(templ)--templ;pending=0;}
        else if(pending=='{' && c=='|'){++table;pending=0;}
        else if(pending=='|' && c=='}'){if(table)--table;pending=0;}
        else pending=c;
    }
    void observe(unsigned id,const History192&,const Features&){
        if(!active)return;
        unsigned b=id>=wordCount?id-wordCount:256;
        bool isSpace=id==spaceMarker;
        bool format=isSpace || (id>=wordCount && punctuation(b));
        if(format){
            unsigned wire=isSpace?groups::spaceFlip:b;exact.write(key).add(wire);coarse.write(shortKey).add(wire);++trained;
            if(!isSpace && !std::isspace(b))lastPunct=b;
        }
        if(isSpace){lastKind=1;return;}
        if(id<wordCount){text('a');lastKind=0;return;}
        if(!quoted && b==digraph::escape){quoted=true;return;}
        if(!quoted && digraph::alphabet.inverse[b]>=0)b='a';quoted=false;
        text(b);
        lastKind=(b=='\n'||b=='\r')?2:b==' '?1:punctuation(b)?3:(b>='0'&&b<='9')?4:5;
    }
};
#ifndef WIKI_NUMERIC_UTF
using Selected=Markup;
#endif
#endif
#ifdef WIKI_NUMERIC_UTF
struct Utf:State {
    using Row=Counts<256>;
    Tagged<Row> exact{8192},coarse{1024};
    const Row* detail=nullptr;const Row* general=nullptr;
    unsigned need=0,lead=0,lastByte=0,length=0,script=0,codepoint=0,low=128,high=191,node=0;
    uint32_t key=0,shortKey=0;
    void start(const History192&,const Features&,const Features::Query&,bool enabled){
        active=enabled;total=0;
        shortKey=combine(need*64+std::min(15u,length),need?lead:script);
        key=combine(shortKey,lastByte);detail=exact.get(key);general=coarse.get(shortKey);
    }
    void select(unsigned q){
        node=q;total=0;if(!active || (!length && !need))return;
        gate=&gates[(q==0?0:32)+std::min(3u,need)*4+std::min(3u,length)];
        if(!q && need){total=64;++predicted;return;}
        if(q!=290)return;
        total=(general?general->total:0)+4*(detail?detail->total:0)+(need?high-low+1:128);
        ++predicted;
    }
    unsigned mass(unsigned c)const{
        if(!node)return c==groups::escaped?64:0;
        return (general?general->count[c]:0)+4*(detail?detail->count[c]:0)+(need?(c>=low&&c<=high):(c>=128));
    }
    void observe(unsigned id,const History192&,const Features&){
        if(!active)return;
        if(id<wordCount+128 || id>=wordCount+256){need=length=lastByte=lead=codepoint=script=0;return;}
        unsigned c=id-wordCount;
        if(length || need){coarse.write(shortKey).add(c);exact.write(key).add(c);++trained;}
        if(need && c>=low && c<=high){
            codepoint=(codepoint<<6)|(c&63);--need;low=128;high=191;
            if(!need)script=std::min(4095u,codepoint>>6);
        }else{
            need=c>=194 && c<=223?1:c>=224 && c<=239?2:c>=240 && c<=244?3:0;
            lead=c;codepoint=need?c&(0x7fu>>(need+1)):0;
            low=c==224?160:c==240?144:128;high=c==237?159:c==244?143:191;
        }
        length=std::min(15u,length+1);lastByte=c;
    }
};
#ifdef WIKI_MARKUP_CONTEXT
struct NumbersAndMarkup {
    Utf utf;
    Markup markup;
    bool useUtf=false;
    int* gate=nullptr;
    unsigned total=0;
    uint64_t predicted=0,trained=0;
    void start(const History192& h,const Features& f,const Features::Query& q,bool enabled){
        utf.start(h,f,q,enabled);markup.start(h,f,q,enabled);
    }
    void select(unsigned node){
        utf.select(node);useUtf=utf.total!=0;
        if(useUtf){total=utf.total;gate=utf.gate;}
        else{markup.select(node);total=markup.total;gate=markup.gate;}
        predicted+=total!=0;
    }
    unsigned mass(unsigned symbol)const{return useUtf?utf.mass(symbol):markup.mass(symbol);}
    void observe(unsigned id,const History192& h,const Features& f){
        utf.observe(id,h,f);markup.observe(id,h,f);trained=utf.trained+markup.trained;
    }
};
using Selected=NumbersAndMarkup;
#else
using Selected=Utf;
#endif
#endif
#ifdef WIKI_MISS_GROUPS
#ifdef WIKI_GRAMMAR_CLASSES
#include "miss_grammar.hpp"
using Selected=MissGrammar;
#else
#include "miss_groups.hpp"
using Selected=MissGroups;
#endif
#endif
}
