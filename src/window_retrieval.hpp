// Included inside namespace window. The live query is capped at the configured
// token window. Its index is learned from previously decoded continuation sites.
struct Retrieval {
    static constexpr uint32_t capacity=1u<<22, mask=capacity-1, prime=0x9e3779b1u;
    static constexpr std::array<unsigned,4> orders{2,5,12,32};
    struct Sites {uint32_t recent=0,previous=0;};
    struct Hit {uint32_t pos=0,id=0;unsigned length=0,bucket=0;};
    struct Accuracy {uint16_t hits=0,seen=0;};
    std::vector<uint32_t> tokens=std::vector<uint32_t>(capacity),prefix=std::vector<uint32_t>(capacity);
    std::array<std::vector<Sites>,4> index;
    std::array<Sites*,4> write{};
    std::array<uint32_t,maxWindow+1> power{};
    std::array<Hit,12> hits{};
    std::array<Hit,2> carry{};
    unsigned used=0,carried=0,strongest=0,limit;
    std::array<Accuracy,28> accuracy{};
    uint64_t deepPositions=0,fullPositions=0;
    explicit Retrieval(unsigned l):limit(l){
        power[0]=1;for(size_t i=1;i<power.size();++i)power[i]=power[i-1]*prime;
        for(size_t i=0;i<orders.size();++i)if(orders[i]<=limit)index[i].resize(1u<<(i==0?19:18));
    }
    uint32_t suffix(uint32_t pos,unsigned len)const{
        return prefix[pos&mask]-power[len]*prefix[(pos-len)&mask];
    }
    static unsigned lengthBin(unsigned n){
        if(n<5)return n-2;
        if(n<8)return 3;
        if(n<12)return 4;
        if(n<16)return 5;
        if(n<24)return 6;
        if(n<32)return 7;
        if(n<48)return 8;
        if(n<64)return 9;
        if(n<96)return 10;
        if(n<128)return 11;
        if(n<192)return 12;
        return 13;
    }
    void candidate(uint32_t now,uint32_t old,unsigned seed,Scratch& forecast) {
        if(!old || old>=now || now-old>=capacity-maxWindow-1 || old<seed)return;
        for(unsigned i=0;i<used;++i)if(hits[i].pos==old)return;
        if(suffix(now,seed)!=suffix(old,seed))return;
        unsigned maximum=std::min(old,limit),n=seed;
        // Direct comparison usually stops after one token. Hash long runs in
        // blocks, then resolve the remaining partial block exactly.
        while(n<maximum){
            if(n+16<=maximum && suffix(now,n+16)==suffix(old,n+16)){n+=16;continue;}
            if(tokens[(now-n-1)&mask]!=tokens[(old-n-1)&mask])break;
            ++n;
        }
        unsigned bin=lengthBin(n),b=2*bin+(now-old<=limit);
        const auto& a=accuracy[b];
        unsigned expected=std::min(50u,3u+3u*bin);
        unsigned vote=std::min(48000u,220u*(a.hits+expected)/(a.seen-a.hits+6));
        unsigned id=tokens[old&mask];
        hits[used++]={old,id,n,b};forecast.vote(id,vote);strongest=std::max(strongest,n);
    }
    void predict(uint32_t now,Scratch& out) {
        used=0;strongest=0;write={};
        for(unsigned i=0;i<carried;++i)candidate(now,carry[i].pos,carry[i].length,out);
        for(size_t i=0;i<orders.size();++i){
            auto len=orders[i];if(len>limit || now<len)continue;
            auto key=groups::hash(suffix(now,len));auto& table=index[i];auto& s=table[key&(table.size()-1)];
            write[i]=&s;candidate(now,s.recent,len,out);
            if(i<2)candidate(now,s.previous,len,out);
        }
        deepPositions+=strongest>=16;fullPositions+=strongest==limit;
    }
    void learn(uint32_t now,uint32_t actual){
        carried=0;
        for(unsigned i=0;i<used;++i){
            auto h=hits[i];auto& a=accuracy[h.bucket];
            if(a.seen>=4096){a.seen/=2;a.hits/=2;}
            ++a.seen;if(h.id!=actual)continue;++a.hits;
            h.pos+=1;h.length=std::min(limit,h.length+1);
            if(carried<carry.size())carry[carried++]=h;
            else {unsigned weak=carry[0].length<carry[1].length?0:1;if(h.length>carry[weak].length)carry[weak]=h;}
        }
        for(auto slot:write)if(slot){slot->previous=slot->recent;slot->recent=now;}
        tokens[now&mask]=actual;prefix[(now+1)&mask]=prefix[now&mask]*prime+actual+1;
    }
    unsigned strength()const{
        return strongest<2?0:strongest<4?1:strongest<8?2:strongest<16?3:strongest<32?4:strongest<64?5:6;
    }
};
