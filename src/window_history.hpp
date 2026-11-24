// The wire alphabet is exactly the existing 65,536-word format. An escaped byte
// and a complete multi-byte word ID each advance the prediction window once.
#include <array>
#include <limits>
#include <memory>

namespace window {
constexpr uint32_t wordCount = 65536, spaceMarker = wordCount + 256, ids = spaceMarker + 1;
constexpr uint32_t noToken = UINT32_MAX;
constexpr unsigned nodes = 291, maxWindow = 192, ringSize = 256;
struct Recipe {
    View name; unsigned length; bool followers, firstOnly, topics;
    bool retrieval=false, threeOnly=false;
};
constexpr std::array<Recipe,13> recipes{{
    {"off",0,false,false,false}, {"cache16",16,false,false,false}, {"cache192",192,false,false,false},
    {"learn16",16,true,false,true}, {"learn192",192,true,false,true}, {"first192",192,true,true,true},
    {"sequence192",192,true,false,false}, {"learn64",64,true,false,true},
    {"retrieve16",16,true,false,true,true}, {"retrieve192",192,true,false,true,true},
    {"retrieve-first192",192,true,true,true,true}, {"retrieve64",64,true,false,true,true},
    {"retrieve-three192",192,true,false,true,true,true}
}};

struct Wire {
    uint8_t n, a, b, c;
    explicit Wire(unsigned id) : n(1),a(0),b(0),c(0) {
        if (id<80) a=uint8_t(128+id);
        else if (id<8272) { unsigned v=id-80;n=2;a=uint8_t(208+(v>>8));b=uint8_t(v); }
        else if (id<wordCount) { unsigned v=id-8272;n=3;a=240;b=uint8_t(v>>8);c=uint8_t(v); }
        else if (id==spaceMarker) a=groups::spaceFlip;
        else if (id-wordCount<128) a=uint8_t(id-wordCount);
        else { n=2;a=groups::escaped;b=uint8_t(id-wordCount); }
    }
    unsigned secondNode() const { return a==groups::escaped?290u:1u+a-208; }
};

struct Parser {
    unsigned node=0;
    uint32_t take(unsigned byte) {
        unsigned from=node;node=0;
        if (!from) {
            if (byte<128) return wordCount+byte;
            if (byte<208) return byte-128;
            if (byte<=240) node=1+byte-208;
            else if (byte==groups::spaceFlip) return spaceMarker;
            else if (byte==groups::escaped) node=290;
            else throw std::runtime_error("Invalid token prefix");
        } else if (from<=32) return 80+256*(from-1)+byte;
        else if (from==33) node=34+byte;
        else if (from==290) {check(byte>=128,"Noncanonical byte escape");return wordCount+byte;}
        else { auto id=8272+256*(from-34)+byte;check(id<wordCount,"Invalid word ID");return id; }
        return noToken;
    }
};

struct Histogram {
    std::array<uint32_t,nodes> total{};
    std::array<std::array<uint32_t,256>,nodes> freq{};
    void add(unsigned id,int delta) {
        Wire w(id);total[0]+=delta;freq[0][w.a]+=delta;
        if(w.n>=2){auto q=w.secondNode();total[q]+=delta;freq[q][w.b]+=delta;}
        if(w.n==3){unsigned q=34+w.b;total[q]+=delta;freq[q][w.c]+=delta;}
    }
};
struct Cell { uint32_t epoch=0, value=0; };
struct Scratch {
    uint32_t epoch=0;
    std::array<Cell,nodes> total{};
    std::vector<Cell> freq=std::vector<Cell>(nodes*256), tokens=std::vector<Cell>(ids);
#ifdef WIKI_STEM_CONTEXTS
    // All eight lexical contexts can contribute ten distinct words each.
    std::array<uint32_t,80> touched{};
#else
    std::array<uint32_t,64> touched{};
#endif
    unsigned used=0;
    void reset(){++epoch;used=0;}
    unsigned get(const Cell& c) const {return c.epoch==epoch?c.value:0;}
    void add(Cell& c,unsigned x){if(c.epoch!=epoch){c.epoch=epoch;c.value=0;}c.value+=x;}
    void branch(unsigned q,unsigned c,unsigned x){add(total[q],x);add(freq[256*q+c],x);}
    void vote(unsigned id,unsigned x) {
        if(!x)return;
        if(tokens[id].epoch!=epoch){
#ifdef WIKI_STEM_CONTEXTS
            check(used<touched.size(),"Too many distinct stem candidates");
#endif
            touched[used++]=id;
        }
        add(tokens[id],x);Wire w(id);branch(0,w.a,x);
        if(w.n>=2)branch(w.secondNode(),w.b,x);
        if(w.n==3)branch(34+w.b,w.c,x);
    }
};

#include "window_retrieval.hpp"

// A decaying shortlist stores alternatives observed after the same prior context.
struct Follows {
    static constexpr unsigned capacity=5;
    uint32_t tag=0;
    std::array<uint32_t,capacity> id{};
    std::array<uint8_t,capacity> freq{};
    uint8_t total=0,n=0;
    void observe(unsigned actual) {
        if(total>=224) {
            unsigned live=0;
            for(unsigned i=0;i<n;++i) if(freq[i]>=2) {id[live]=id[i];freq[live++]=freq[i]/2;}
            n=uint8_t(live);total/=2;
        }
        ++total; unsigned least=0;
        for(unsigned i=0;i<n;++i) {
            if(id[i]==actual){++freq[i];return;}
            if(freq[i]<freq[least])least=i;
        }
        if(n<capacity){id[n]=actual;freq[n++]=1;}
        else if(freq[least]<=2){id[least]=actual;freq[least]=1;}
        else --freq[least];
    }
    void votes(Scratch& out,unsigned budget) const {
        unsigned denom=total+2*n+6;
        for(unsigned i=0;i<n;++i)out.vote(id[i],(budget*freq[i])/denom);
    }
};
struct Table {
    std::vector<Follows> slots;
    explicit Table(size_t n):slots(n){}
    Follows* row(uint32_t key) {
        auto& r=slots[key&(slots.size()-1)];
        if(r.tag!=key){r.tag=key;r.n=0;r.total=0;}
        return &r;
    }
};
struct Entry { uint32_t token=0, anchor=noToken, anchorAt=0; };
struct Rank { uint32_t id=noToken, score=0; };
struct History192 {
    static bool needsRanks(const Recipe& r){
#ifdef WIKI_MISS_GROUPS
        return true;
#else
        return r.firstOnly || r.threeOnly;
#endif
    }
    Recipe recipe;
    unsigned factor=0;uint32_t position=0;
    std::array<Entry,ringSize> ring{};
    Histogram cache;
    Scratch hints;
    std::vector<uint16_t> counts=std::vector<uint16_t>(ids);
    std::vector<uint32_t> next,prev;
    std::array<uint32_t,maxWindow+1> heads{};
    unsigned maxCount=0;
    std::vector<Follows> single;
    Table pair,quad,topic;
    std::array<Follows*,6> training{};
    unsigned nTrain=0;
    std::unique_ptr<Retrieval> retrieval;
    std::array<Rank,3> truncated{};
    explicit History192(Recipe r):recipe(r),factor(r.length?maxWindow/r.length:0),
        next(needsRanks(r)?ids:0),prev(needsRanks(r)?ids:0),single(r.followers?ids:0),
        pair(r.followers?1<<19:0),quad(r.followers?1<<19:0),topic(r.topics?1<<19:0) {
        heads.fill(noToken);
        if(r.retrieval)retrieval=std::make_unique<Retrieval>(r.length);
    }
    const Entry& back(unsigned offset) const {return ring[(position-1-offset)&(ringSize-1)];}
    void adjust(unsigned id,int delta) {
        unsigned old=counts[id],now=unsigned(int(old)+delta);counts[id]=uint16_t(now);
        cache.add(id,delta);
        if(!needsRanks(recipe))return;
        if(old){
            if(prev[id]==noToken)heads[old]=next[id];else next[prev[id]]=next[id];
            if(next[id]!=noToken)prev[next[id]]=prev[id];
        }
        if(now){
            next[id]=heads[now];prev[id]=noToken;
            if(heads[now]!=noToken)prev[heads[now]]=id;
            heads[now]=id;maxCount=std::max(maxCount,now);
        }
        while(maxCount && heads[maxCount]==noToken)--maxCount;
    }
    uint32_t score(unsigned id) const {return factor*counts[id]+hints.get(hints.tokens[id]);}
    static bool better(Rank a,Rank b){return a.score>b.score || (a.score && a.score==b.score && a.id<b.id);}
    void use(Follows* r,unsigned strength) {
        for(unsigned i=0;i<nTrain;++i)if(training[i]==r)return;
        training[nTrain++]=r;r->votes(hints,strength);
    }
    void begin() {
        hints.reset();nTrain=0;
        if(recipe.followers && position) {
            unsigned last=back(0).token;
            use(&single[last],160);
            if(position>=2){
                uint32_t key=groups::hash(last+0x9e3779b1u*back(1).token);
                use(pair.row(key),280);
                if(position>=4)use(quad.row(groups::hash(key+back(2).token*0x85ebca6bu+back(3).token*0xc2b2ae35u)),460);
            }
            if(recipe.topics) {
                std::array<unsigned,3> ages{2,recipe.length/6,recipe.length/2};
                for(unsigned age:ages) if(position>age) {
                    const auto& e=back(age);
                    if(e.anchor!=noToken && position-e.anchorAt<=recipe.length)
                        use(topic.row(groups::hash(last*0x27d4eb2du+e.anchor)),92);
                }
            }
        }
        if(retrieval)retrieval->predict(position,hints);
        if(needsRanks(recipe) && !recipe.firstOnly){
            truncated={};
            auto consider=[&](unsigned id){
                for(auto k:truncated)if(k.id==id)return;
                Rank candidate{id,score(id)};for(auto& k:truncated)if(better(candidate,k))std::swap(candidate,k);
            };
            for(unsigned i=0;i<hints.used;++i)consider(hints.touched[i]);
            for(unsigned k=maxCount;k && k*factor>=truncated.back().score;--k)
                for(auto id=heads[k];id!=noToken;id=next[id])consider(id);
        }else if(recipe.firstOnly){
            Rank best;
            for(unsigned id=heads[maxCount];id!=noToken;id=next[id]){
                Rank r{id,score(id)};if(better(r,best))best=r;
            }
            for(unsigned i=0;i<hints.used;++i){auto id=hints.touched[i];Rank r{id,score(id)};if(better(r,best))best=r;}
            truncated={};truncated[0]=best;
        }
    }
    std::array<Rank,3> topThree() const {
        std::array<Rank,3> ranks{};
        auto test=[&](unsigned id) {
            for(auto k:ranks)if(k.id==id)return;
            Rank r{id,score(id)};for(auto& k:ranks)if(better(r,k))std::swap(r,k);
        };
        for(unsigned i=0;i<std::min(position,recipe.length);++i)test(back(i).token);
        for(unsigned i=0;i<hints.used;++i)test(hints.touched[i]);
        return ranks;
    }
    void observe(unsigned actual) {
        if(retrieval)retrieval->learn(position,actual);
        for(unsigned i=0;i<nTrain;++i)training[i]->observe(actual);
        if(recipe.length) {
            if(position>=recipe.length)adjust(back(recipe.length-1).token,-1);
            adjust(actual,1);
        }
        Entry e=position?back(0):Entry{};e.token=actual;
        if(actual>=80 && actual<wordCount){e.anchor=actual;e.anchorAt=position;}
        ring[position&(ringSize-1)]=e;++position;
    }
    static std::pair<unsigned,unsigned> branchOf(unsigned id,unsigned node) {
        if(id==noToken)return {0,0};
        Wire w(id);
        if(!node)return {1,w.a};
        if(w.n>=2 && node==w.secondNode())return {1,w.b};
        if(w.n==3 && node==34u+w.b)return {1,w.c};
        return {0,0};
    }
};

// Several complete token IDs can have the same wire prefix. Add their mass
// before the first byte, and condition on that prefix for the following bytes.
struct Evidence {
    const uint32_t* cache;
    const Cell* learned;
    uint32_t epoch,total;
    unsigned factor,strength,support,nKept=0;
    std::array<unsigned,3> keptBytes{},keptWeights{};
    bool limited;
    Evidence(const History192& h,unsigned q):cache(h.cache.freq[q].data()),learned(&h.hints.freq[256*q]),
        epoch(h.hints.epoch),total(h.cache.total[q]*h.factor+h.hints.get(h.hints.total[q])),
        factor(h.factor),strength(h.retrieval?h.retrieval->strength():0),support(total),limited(h.recipe.firstOnly||h.recipe.threeOnly) {
        if(limited){
            total=0;
            for(auto rank:h.truncated){
                auto b=h.branchOf(rank.id,q);if(!b.first)continue;
                unsigned weight=h.retrieval?rank.score:1;
                keptBytes[nKept]=b.second;keptWeights[nKept++]=weight;total+=weight;
            }
            if(!h.retrieval)support=total;
        }
    }
    unsigned operator()(unsigned symbol) const {
        if(limited){
            unsigned value=0;for(unsigned i=0;i<nKept;++i)if(keptBytes[i]==symbol)value+=keptWeights[i];
            return value;
        }
        auto c=learned[symbol];
        return cache[symbol]*factor+(c.epoch==epoch?c.value:0);
    }
};
} // namespace window
