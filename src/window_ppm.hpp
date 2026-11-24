// PPMd-I model updates follow Dmitry Shkarin and Igor Pavlov's public-domain
// implementation in ppmd-sdk-25.01. Both directions use the same model walk.
#include <cmath>
extern "C" void Ppmd8_UpdateModel(CPpmd8*);

namespace window {
constexpr uint64_t normalizeAt=UINT64_C(1)<<56;
struct RangeOut {
    static constexpr bool reading=false;
    Bytes bytes;uint64_t range=UINT64_MAX,step=0,pending=1;
    __uint128_t low=0;uint8_t cache=0;
    void shift(){
        uint64_t lo=uint64_t(low);unsigned carry=unsigned(low>>64);
        if(lo<UINT64_C(0xff00000000000000) || carry){
            unsigned c=cache;
            do{bytes.push_back(char(c+carry));c=255;}while(--pending);
            cache=uint8_t(lo>>56);
        }
        ++pending;low=uint64_t(lo<<8);
    }
    uint64_t threshold(uint64_t total){step=range/total;return 0;}
    void commit(uint64_t from,uint64_t width){
        low+=step*from;range=step*width;
        while(range<normalizeAt){range<<=8;shift();}
    }
    Bytes finish(){for(unsigned i=0;i<9;++i)shift();return std::move(bytes);}
};
struct RangeIn {
    static constexpr bool reading=true;
    Reader in;uint64_t range=UINT64_MAX,code=0,step=0;
    explicit RangeIn(View stream):in{stream}{
        check(in.byte()==0,"Bad range prefix");for(unsigned i=0;i<8;++i)code=(code<<8)|in.byte();
    }
    uint64_t threshold(uint64_t total){
        step=range/total;auto value=code/step;check(value<total,"Bad range code");return value;
    }
    void commit(uint64_t from,uint64_t width){
        code-=step*from;range=step*width;
        while(range<normalizeAt){range<<=8;code=(code<<8)|in.byte();}
    }
    void finish(){check(code==0 && in.at==in.data.size(),"Range stream did not end exactly");}
};
constexpr unsigned weightScale=1024;
struct Gate {
    int32_t estimate=64*16384;
    unsigned weight()const{return unsigned(estimate+8192)>>14;}
    void learn(unsigned posterior) {
        estimate+=(int32_t(posterior*16384)-estimate)/256;
        estimate=std::clamp(estimate,2*16384,992*16384);
    }
};
struct GateBank {
    std::array<Gate,7*5*16*8> gates{};
    Gate& select(unsigned node,unsigned confidence,unsigned forecasts,unsigned strength){
        unsigned kind=node==0?0:node<=32?1:node==33?2:node==290?4:3;
        unsigned bin=std::min(7u,31u-unsigned(__builtin_clz(forecasts)));
        return gates[((strength*5+kind)*16+confidence)*8+bin];
    }
};
struct Cost {double base=0,combined=0;};

// Each PPM node offers symbols plus an escape to the shorter context. On escape
// we condition both predictors on the remaining symbols and update their odds.
struct Branch {
    uint64_t ppmUnit,contextUnit,total;
    unsigned baseTotal;
    Branch(unsigned base,unsigned available,unsigned prior):baseTotal(base) {
        if(!prior || !available){ppmUnit=1;contextUnit=0;total=base;return;}
        ppmUnit=uint64_t(weightScale-prior)*available;
        contextUnit=uint64_t(prior)*base;
        total=uint64_t(weightScale)*available*base;
    }
    uint64_t mass(unsigned base,unsigned extra)const{return base*ppmUnit+extra*contextUnit;}
    template<class Range> unsigned take(Range& range,uint64_t from,unsigned base,unsigned extra,Cost* cost)const{
        auto width=mass(base,extra);check(width>0 && from+width<=total,"Invalid arithmetic interval");
        range.commit(from,width);
        if(cost){cost->base+=std::log2(double(baseTotal)/base);cost->combined+=std::log2(double(total)/width);}
        if(!contextUnit || !extra)return 0;
        return std::min(weightScale-1,unsigned((uint64_t(weightScale)*extra*contextUnit+width/2)/width));
    }
};

#ifdef WIKI_WORD_FINAL
#include "word_final.hpp"
#endif
struct ByteModel {
#ifdef WIKI_WORD_FINAL
    WordFinal final;
#endif
    ppm::Model model;
    GateBank bank;
#ifdef WIKI_WORD_EXPERTS
    BinaryCalibration* calibration=nullptr;
    template<class Range> unsigned transfer(Range& rc,int actual,std::nullptr_t,unsigned node,Cost* cost) {
        return transfer(rc,actual,static_cast<const Evidence*>(nullptr),node,cost);
    }
    template<class Range,class Forecast> unsigned transfer(Range& rc,int actual,const Forecast* evidence,unsigned node,Cost* cost) {
#else
    template<class Range> unsigned transfer(Range& rc,int actual,const Evidence* evidence,unsigned node,Cost* cost) {
#endif
        auto* p=&model.state;
        unsigned remaining=evidence?evidence->total:0;
        unsigned prior=0;Gate* gate=nullptr;
        auto initGate=[&](unsigned quality) {
            if(remaining){gate=&bank.select(node,quality,evidence->support,evidence->strength);prior=gate->weight();}
        };
        auto extra=[&](unsigned sym){return prior?(*evidence)(sym):0u;};
        auto learn=[&](unsigned post){if(gate)gate->learn(post);};
        std::array<uint8_t,256> masked;
        auto* ctx=p->MinContext;
        unsigned excluded=ctx->NumStats;
        if(!ctx->NumStats){
            auto* s=Ppmd8Context_OneState(ctx);unsigned symbol=s->Symbol;
            auto* prob=Ppmd8_GetBinSumm(p);unsigned base=*prob,scale=PPMD_BIN_SCALE;
            initGate(std::min(7u,base>>11));
            unsigned e=extra(symbol);Branch b(scale,remaining,prior);
            uint64_t first=b.mass(base,e);
#ifdef WIKI_WORD_EXPERTS
#ifdef WIKI_WORD_FINAL
            WordFinal::Partition<Range> partition(rc,final);
            BinaryCalibration::Step<decltype(partition)> binary(partition,calibration,first,b.total,node,evidence?evidence->strength:0
#else
            BinaryCalibration::Step<Range> binary(rc,calibration,first,b.total,node,evidence?evidence->strength:0
#endif
#ifdef WIKI_ONLINE_WORD_NET
                                                  ,symbol
#endif
            );
#else
            auto& binary=rc;
#endif
#ifdef WIKI_WORD_FINAL
            partition.start(binary.error?binary.corrected:first,binary.error?BinaryCalibration::unit:b.total);
#endif
            uint64_t target=binary.threshold(b.total);
            unsigned updated=PPMD_UPDATE_PROB_1(base);
            if(Range::reading?target<first:actual==int(symbol)){
                auto post=b.take(binary,0,base,e,cost);*prob=UInt16(updated+(1<<PPMD_INT_BITS));
                p->FoundState=s;p->PrevSuccess=1;++p->RunLength;
                s->Freq=Byte(s->Freq+(s->Freq<196));
                auto* next=Ppmd8_GetContext(p,Ppmd_GET_SUCCESSOR(s));
                if(!p->OrderFall && reinterpret_cast<const Byte*>(next)>=p->UnitsStart)p->MaxContext=p->MinContext=next;
                else Ppmd8_UpdateModel(p);
                learn(post);return symbol;
            }
            prior=b.take(binary,first,scale-base,remaining-e,cost);remaining-=e;
            *prob=UInt16(updated);p->InitEsc=p->ExpEscape[updated>>10];p->PrevSuccess=0;
            masked.fill(0);masked[symbol]=1;
        }else{
            auto* states=Ppmd8_GetStats(p,ctx);unsigned n=ctx->NumStats+1,base=ctx->Union2.SummFreq;
            initGate(8+std::min(7u,8u*states[0].Freq/base));
            Branch b(base,remaining,prior);
#ifdef WIKI_WORD_FINAL
            WordFinal::Partition<Range> choice(rc,final);choice.start(b.mass(states[0].Freq,extra(states[0].Symbol)),b.total);
#else
            auto& choice=rc;
#endif
            auto target=choice.threshold(b.total);uint64_t before=0;
            unsigned baseSeen=0,extraSeen=0;
            for(unsigned i=0;i<n;++i){
                auto& s=states[i];unsigned e=extra(s.Symbol);auto width=b.mass(s.Freq,e);
                if(Range::reading?target<before+width:actual==int(s.Symbol)){
                    auto post=b.take(choice,before,s.Freq,e,cost);unsigned symbol=s.Symbol;p->FoundState=&s;
                    if(i){p->PrevSuccess=0;Ppmd8_Update1(p);}else Ppmd8_Update1_0(p);
                    learn(post);return symbol;
                }
                before+=width;baseSeen+=s.Freq;extraSeen+=e;
            }
            prior=b.take(choice,before,base-baseSeen,remaining-extraSeen,cost);remaining-=extraSeen;
            p->PrevSuccess=0;masked.fill(0);for(unsigned i=0;i<n;++i)masked[states[i].Symbol]=1;
        }
        for(;;){
            do{
                ++p->OrderFall;check(ctx->Suffix!=0,"Unexpected PPM end marker");
                ctx=Ppmd8_GetContext(p,ctx->Suffix);
            }while(ctx->NumStats==excluded);
            p->MinContext=ctx;UInt32 escape;
            auto* see=Ppmd8_MakeEscFreq(p,excluded,&escape);
            auto* states=Ppmd8_GetStats(p,ctx);unsigned n=ctx->NumStats+1;
            unsigned baseTotal=escape;
            for(unsigned i=0;i<n;++i)if(!masked[states[i].Symbol])baseTotal+=states[i].Freq;
            Branch b(baseTotal,remaining,prior);
#ifdef WIKI_WORD_FINAL
            unsigned first=0;while(first<n && masked[states[first].Symbol])++first;
            check(first<n,"Empty suffix partition");WordFinal::Partition<Range> choice(rc,final);
            choice.start(b.mass(states[first].Freq,extra(states[first].Symbol)),b.total);
#else
            auto& choice=rc;
#endif
            auto target=choice.threshold(b.total);uint64_t before=0;unsigned extraSeen=0;
            for(unsigned i=0;i<n;++i){
                auto& s=states[i];if(masked[s.Symbol])continue;
                unsigned e=extra(s.Symbol);auto width=b.mass(s.Freq,e);
                if(Range::reading?target<before+width:actual==int(s.Symbol)){
                    unsigned symbol=s.Symbol;auto post=b.take(choice,before,s.Freq,e,cost);
                    Ppmd_See_UPDATE(see)
                    p->FoundState=&s;Ppmd8_Update2(p);learn(post);return symbol;
                }
                before+=width;extraSeen+=e;
            }
            prior=b.take(choice,before,escape,remaining-extraSeen,cost);remaining-=extraSeen;
            see->Summ=UInt16(see->Summ+baseTotal);
            for(unsigned i=0;i<n;++i)masked[states[i].Symbol]=1;
            excluded=ctx->NumStats;
        }
    }
};
} // namespace window
