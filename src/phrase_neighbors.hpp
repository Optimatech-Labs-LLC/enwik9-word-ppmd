namespace phrase {
// Test membership of the actual word in the original expert's top three.
// This runs after the word arrives. The answer affects future forecasts only.
struct OriginalRank {
    std::array<unsigned,193> bins{};
    unsigned highest=0;
    bool missed(const History192& h,unsigned target) const {
        unsigned score=h.score(target);
        if(!score)return true;
        unsigned better=0;
        for(unsigned i=0;i<h.hints.used;++i){
            auto id=h.hints.touched[i],s=h.score(id);
            if((s>score || (s==score && id<target)) && ++better==3)return true;
        }
        if(highest*h.factor<score)return false;
        std::array<unsigned,3> distinct{};unsigned used=0;
        for(unsigned i=0;i<std::min(h.position,h.recipe.length);++i){
            unsigned id=h.back(i).token,s=h.counts[id]*h.factor;
            if(h.hints.get(h.hints.tokens[id]) || s<score || (s==score && id>=target))continue;
            if(std::find(distinct.begin(),distinct.begin()+used,id)!=distinct.begin()+used)continue;
            if(++better==3)return true;
            distinct[used++]=id;
        }
        return false;
    }
    void observe(const History192& h,unsigned id){
        if(h.position>=h.recipe.length){
            auto old=h.back(h.recipe.length-1).token;if(old==id)return;
            auto n=h.counts[old];--bins[n];if(n>1)++bins[n-1];
        }
        unsigned n=h.counts[id];if(n)--bins[n];++bins[n+1];highest=std::max(highest,n+1);
        while(highest&&!bins[highest])--highest;
    }
};

struct WordCache {
    Histogram distribution;
    std::vector<uint16_t> ring,counts=std::vector<uint16_t>(wordCount);
    unsigned clock=0,cap;
    WordCache(unsigned window,unsigned countLimit):ring(window),cap(countLimit){}
    void observe(unsigned id){
        auto& slot=ring[clock&(ring.size()-1)];
        if(clock>=ring.size()){
            if(--counts[slot]<cap)distribution.add(slot,-1);
        }
        slot=uint16_t(id);++clock;
        if(counts[id]++<cap)distribution.add(id,1);
    }
};
struct SimilarSites {
    static constexpr unsigned capacity=1u<<21,mask=capacity-1,slots=1u<<18;
    struct Hits {uint32_t yes=0,total=0;};
    struct Candidate {uint32_t pos,id,bin;};
    std::array<std::vector<uint32_t>,4> index;
    std::vector<uint32_t> raw=std::vector<uint32_t>(capacity);
    std::vector<uint8_t> pastMiss=std::vector<uint8_t>(capacity);
    std::array<uint32_t*,4> updates{};std::array<Candidate,4> candidates{};
    std::array<Hits,32> rates{};
    unsigned used=0;
    uint64_t evaluated=0,credited=0;
    Scratch forecast;
    SimilarSites(){for(auto& i:index)i.resize(slots);}
    void begin(const History192& h,const Features& feature,bool missesOnly){
        forecast.reset();updates={};used=0;
        if(h.position<8)return;
        std::array<Word,8> context;
        for(unsigned i=0;i<8;++i)context[i]=feature.words[h.back(i).token];
        auto key=[&](unsigned a,unsigned b,unsigned c){
            return combine(context[a].folded,combine(context[b].folded,context[c].folded));
        };
        std::array<uint32_t,4> keys{key(0,2,3),key(1,2,4),key(0,1,4),key(0,3,5)};
        for(unsigned i=0;i<keys.size();++i){
            auto& cell=index[i][keys[i]&(slots-1)];updates[i]=&cell;
            auto old=cell;
            if(old<8 || h.position-old>=capacity-8 || h.retrieval->strongest>=8)continue;
            unsigned next=raw[old&mask];
            if(next>=wordCount || (missesOnly&&!pastMiss[old&mask]))continue;
            bool duplicate=false;for(unsigned j=0;j<used;++j)duplicate|=candidates[j].pos==old;
            if(duplicate)continue;
            unsigned matches=0,shapes=0,different=0;bool lastMatches=false;
            for(unsigned j=0;j<8;++j){
                unsigned previous=raw[(old-1-j)&mask];const auto& other=feature.words[previous];
                bool same=other.folded==context[j].folded;
                matches+=unsigned(same)*(j<4?2:1);shapes+=other.shape==context[j].shape;
                if(j<5)different+=previous!=h.back(j).token;
                if(!j)lastMatches=same;
            }
            if(!different || matches<5)continue;
            unsigned bin=std::min(7u,matches-5)*4+2*unsigned(lastMatches)+(shapes>=5);
            auto& r=rates[bin];unsigned weight=std::min(10000u,350u*(r.yes+2)/(r.total-r.yes+12));
            forecast.vote(next,weight);candidates[used++]={old,next,bin};++evaluated;
        }
    }
    void observe(unsigned position,unsigned id,bool missed){
        for(unsigned i=0;i<used;++i){
            auto c=candidates[i];auto& r=rates[c.bin];
            if(r.total>=2048){r.yes/=2;r.total/=2;}
            ++r.total;if(c.id==id){++r.yes;++credited;}
        }
        for(auto* slot:updates)if(slot)*slot=position;
        raw[position&mask]=id;pastMiss[position&mask]=uint8_t(missed);
    }
};
struct LocalGains {std::array<int32_t,4> mass{680<<12,140<<12,120<<12,84<<12};};
struct Neighbors {
    OriginalRank rank;
    std::unique_ptr<SimilarSites> sites;
    std::unique_ptr<WordCache> recent,hard;
    std::array<LocalGains,7*4*3> gates{};
    bool only;
    uint64_t misses=0,words=0,sampledOffered=0;
    Neighbors(bool match,bool cache,bool pastMissOnly):only(pastMissOnly){
        if(match)sites=std::make_unique<SimilarSites>();
        if(cache){recent=std::make_unique<WordCache>(2048,8);hard=std::make_unique<WordCache>(32768,5);}
    }
    void begin(const History192& h,const Features& features){if(sites)sites->begin(h,features,only);}
    void observe(const History192& h,unsigned id,bool sampled,const std::array<Rank,3>& guesses){
        bool outside=false;
        if(id<wordCount){
            ++words;outside=rank.missed(h,id);misses+=outside;
            if(sampled){
                bool reference=std::none_of(guesses.begin(),guesses.end(),[id](auto r){return r.id==id;});
                check(outside==reference,"Original top-three classification changed");
                sampledOffered+=outside && sites && sites->forecast.get(sites->forecast.tokens[id])!=0;
            }
            if(recent)recent->observe(id);
            if(hard && outside)hard->observe(id);
        }
        if(sites)sites->observe(h.position,id,outside);
        rank.observe(h,id);
    }
};
template<class Core> struct NearbyForecast {
    const Core& core;Neighbors& history;LocalGains& gains;
    unsigned node,strength,support,total=0;
    std::array<unsigned,4> factor{};
    NearbyForecast(const Core& c,Neighbors& h,unsigned q,unsigned gap):
        core(c),history(h),gains(h.gates[(c.strength*4+(!q?0:q<=32?1:q==33?2:3))*3+std::min(gap,2u)]),
        node(q),strength(c.strength),support(c.support){
        std::array<unsigned,4> sizes{c.total,h.sites?h.sites->forecast.get(h.sites->forecast.total[q]):0,
            h.recent?h.recent->distribution.total[q]:0,h.hard?h.hard->distribution.total[q]:0};
        for(unsigned i=0;i<4;++i)if(sizes[i]){
            factor[i]=std::max(1u,(240*unsigned(gains.mass[i]>>12)+sizes[i]/2)/sizes[i]);
            total+=factor[i]*sizes[i];support=std::max(support,sizes[i]);
        }
    }
    unsigned part(unsigned i,unsigned c)const{
        if(!factor[i])return 0;
        if(!i)return factor[i]*core(c);
        if(i==1)return factor[i]*history.sites->forecast.get(history.sites->forecast.freq[node*256+c]);
        auto& h=i==2?history.recent:history.hard;return factor[i]*h->distribution.freq[node][c];
    }
    unsigned operator()(unsigned c)const{return part(0,c)+part(1,c)+part(2,c)+part(3,c);}
    void observe(unsigned c){
        std::array<unsigned,4> mass{};uint64_t sum=0,active=0;
        for(unsigned i=0;i<4;++i)if(factor[i]){mass[i]=part(i,c);sum+=mass[i];active+=gains.mass[i];}
        if(!sum)return;
        for(unsigned i=0;i<4;++i)if(factor[i]){
            auto& w=gains.mass[i];w=std::clamp(w+(int32_t(active*mass[i]/sum)-w)/256,8<<12,1600<<12);
        }
    }
};
}
