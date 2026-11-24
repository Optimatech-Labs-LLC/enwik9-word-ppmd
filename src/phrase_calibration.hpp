#include <algorithm>
#include <array>
#include <cstdint>

namespace window {
// Learn a residual for a binary PPM choice after the word forecasts have been
// mixed in. All input to the lookup is available before the byte is coded.
struct BinaryCalibration {
#ifdef WIKI_ONLINE_WORD_NET
    word_net::Model* net=nullptr;
#endif
    static constexpr uint32_t unit=1u<<20;
    std::array<int32_t,7*4*2*80> errors{};
    uint64_t decisions=0;
    int32_t& cell(unsigned node,unsigned strength,unsigned p){
        unsigned kind=!node?0:node<=32?1:node==33?2:3;
        unsigned tail=std::min(p,unit-p);
        unsigned power=31-unsigned(__builtin_clz(tail));
        unsigned bin=power*4+((tail-(1u<<power))*4>>power);
        return errors[(((strength*4+kind)*2+(p>=unit/2))*80)+bin];
    }
    template<class Range> struct Step {
        Range& stream;
        uint64_t first;
        uint32_t p=0,corrected=0;
        int32_t* error=nullptr;
#ifdef WIKI_ONLINE_WORD_NET
        word_net::BinaryStep learned;
        word_net::Model* net=nullptr;
#endif
        Step(Range& r,BinaryCalibration* model,uint64_t boundary,uint64_t total,unsigned node,unsigned strength
#ifdef WIKI_ONLINE_WORD_NET
             ,unsigned offered
#endif
             ):
            stream(r),first(boundary) {
            if(!model)return;
            p=uint32_t((__uint128_t(boundary)*unit+total/2)/total);
            if(p<128 || p>unit-128)return;
            error=&model->cell(node,strength,p);++model->decisions;
            int reach=int(std::min(p,unit-p)/2);
            corrected=uint32_t(int(p)+std::clamp(*error/2,-reach,reach));
#ifdef WIKI_ONLINE_WORD_NET
            net=model->net;
            if(net)corrected=net->adjustBinary(learned,node,offered,corrected);
#endif
        }
        uint64_t threshold(uint64_t total){
            if(!error)return stream.threshold(total);
            return stream.threshold(unit)<corrected?0:first;
        }
        void commit(uint64_t from,uint64_t width){
            if(!error){stream.commit(from,width);return;}
            bool hit=from==0;
            stream.commit(hit?0:corrected,hit?corrected:unit-corrected);
            int observed=hit?int(unit):0;
            *error+=(observed-int(p)-*error)/256;
#ifdef WIKI_ONLINE_WORD_NET
            if(net)net->learnBinary(learned,hit);
#endif
        }
    };
};
}
