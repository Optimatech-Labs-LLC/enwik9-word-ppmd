// A causal word model with one hidden layer. Learned embeddings of previous
// words and punctuation feed a binary tree over word-ID prefixes and a parallel
// correction for statistical predictions. Both endpoints learn from decoded data.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

namespace word_net {
#ifdef WIKI_NET_WIDTH
constexpr unsigned dims=WIKI_NET_WIDTH, alphabet=256, wireNodes=291, unit=16384, probUnit=32768;
static_assert(dims==24 || dims==48 || dims==96 || dims==128 || dims==192);
constexpr unsigned biasGroups=dims==24 ? 2 : 4;
#else
constexpr unsigned dims=12, alphabet=256, wireNodes=291, unit=16384, probUnit=32768;
#endif
using Vector=std::array<float,dims>;
inline uint32_t scramble(uint32_t x){
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16);
}
struct Tables {
    std::array<uint16_t,3073> logistic{};
    std::array<float,2049> logCount{};
    std::array<float,1025> odds{};
    Tables(){
        for(unsigned i=0;i<logistic.size();++i){
            double z=(int(i)-1536)/128.;
            logistic[i]=uint16_t(std::clamp(std::lround(probUnit/(1.+std::exp(-z))),1L,long(probUnit-1)));
        }
        for(unsigned i=1;i<logCount.size();++i)logCount[i]=float(std::log(double(i)));
        for(unsigned i=0;i<odds.size();++i)odds[i]=float(std::exp((int(i)-512)/128.));
    }
    unsigned probability(float z)const{return logistic[1536+int(std::clamp(z*128.f,-1536.f,1536.f))];}
};
inline const Tables tables;
inline float dot(const Vector& a,const Vector& b){
    float value=0;for(unsigned i=0;i<dims;++i)value+=a[i]*b[i];return value;
}
struct BranchHead {
    Vector weights{};
    uint16_t left=1,right=1;
    float prior=0;
    unsigned predict(const Vector& h)const{return tables.probability(prior+dot(weights,h));}
    template<bool frequencyPrior=true>
    void train(const Vector& h,Vector& back,unsigned prob,bool first){
        const float error=float(first)-float(prob)/probUnit;
        const float rate=left+right<64?.075f:.027f;
        for(unsigned d=0;d<dims;++d){
            back[d]+=error*weights[d];
            weights[d]=std::clamp(weights[d]+rate*error*h[d],-3.5f,3.5f);
        }
        if constexpr(frequencyPrior){
            if(first)++left;else ++right;
            if(left+right>=2048){left=uint16_t((left+1)/2);right=uint16_t((right+1)/2);}
            prior=tables.logCount[left]-tables.logCount[right];
        }else{
            // Only the number of visits sets the learning rate in this control.
            if(left<64)++left;
            prior=std::clamp(prior+rate*error,-12.f,12.f);
        }
    }
};
#ifdef WIKI_NET_WIDTH
struct BinaryHead {Vector weights{};std::array<float,biasGroups> bias{};};
#else
struct BinaryHead {Vector weights{};float bias=0;};
#endif
struct BinaryStep {BinaryHead* head=nullptr;float p=0;};
struct Model {
    bool useTree,useResidual;
#ifdef WIKI_NET_WIDTH
    unsigned biasGroup=0;
#endif
#ifdef WIKI_CONTENT_CONTEXT
    static constexpr std::array<unsigned,10> sizes{65536,32768,8192,32768,16384,1024,65536,32768,32768,8192};
    std::array<unsigned,4> contentKeys{};
#else
    static constexpr std::array<unsigned,6> sizes{65536,32768,8192,32768,16384,1024};
#endif
    std::array<std::vector<Vector>,sizes.size()> embeddings;
    std::array<Vector*,sizes.size()> active{};
#ifdef WIKI_EIGHT_WORDS
    // Positions four to eight share the third-word table, keeping the budget
    // identical to the three-word control. Position salts distinguish lookups.
    std::array<unsigned,5> olderFolded{};
    std::array<Vector*,5> olderActive{};
    bool useOlder=false;
    template<class Features> void olderWords(const Features& features,unsigned sentinel){
        useOlder=true;
        for(unsigned i=0;i<5;++i){
            auto id=features.lexical[i+3];
            olderFolded[i]=id<sentinel ? features.words[id].folded : sentinel;
        }
    }
#endif
    Vector hidden{},gradient{},derivative{};
#ifdef WIKI_SENTENCE_MEMORY
    // Two causal averages of word embeddings retain sentence and topic clues
    // beyond the explicitly indexed recent words. They change only after a
    // token has been decoded. The readout is trained by the next-token loss.
    Vector shortTrace{},longTrace{},shortGain{},longGain{};
    void remember(unsigned token,unsigned folded){
        if(token<65536){
            const auto& word=embeddings[0][folded&(sizes[0]-1)];
            for(unsigned d=0;d<dims;++d){
                shortTrace[d]+=.30f*(word[d]-shortTrace[d]);
                longTrace[d]+=.055f*(word[d]-longTrace[d]);
            }
        }else if(token==65536+'\n'){
            for(unsigned d=0;d<dims;++d){shortTrace[d]*=.5f;longTrace[d]*=.92f;}
        }
    }
#endif
    std::vector<BranchHead> tree;
    std::vector<BinaryHead> residual;
    struct Cache {uint32_t epoch=0;uint16_t left=0,p=0;};
    std::array<Cache,256> memo{};
    uint32_t epoch=0;unsigned node=0;
    std::array<int32_t,7*4*4> mixture{};
    uint64_t binaryDecisions=0,trainedBytes=0,probes=0,probeWords=0,probeMisses=0,rescued=0,treeHits=0,oldHits=0;
    uint64_t exactPhraseOnly=0,bothHit=0;
    std::array<uint32_t,3> alternative{};
    explicit Model(unsigned mode):useTree(mode&1),useResidual(mode&2),
            tree(useTree?wireNodes*256:0),residual(useResidual?wireNodes*256:0){
        for(unsigned k=0;k<sizes.size();++k){
            embeddings[k].resize(sizes[k]);
            for(unsigned j=0;j<sizes[k];++j)for(unsigned d=0;d<dims;++d)
                embeddings[k][j][d]=float(int(scramble(j*37+d+k*0x91a3u)&255)-128)/896.f;
        }
        mixture.fill(128<<12);
#ifdef WIKI_SENTENCE_MEMORY
        shortGain.fill(1.f);longGain.fill(.75f);
#endif
    }
    void begin(unsigned a,unsigned b,unsigned c,unsigned previous,unsigned shape,unsigned gap){
#ifdef WIKI_NET_WIDTH
        // With two buckets distinguish adjacency from a separator. Four buckets
        // distinguish adjacency, a space, a newline, and all other separators.
        biasGroup=std::min(gap,biasGroups-1);
#endif
        std::array<unsigned,sizes.size()> keys{a,b,c,scramble(a*0x9e3779b1u+b),previous,
            scramble(shape*31+gap)};
#ifdef WIKI_CONTENT_CONTEXT
        std::copy(contentKeys.begin(),contentKeys.end(),keys.begin()+6);
#endif
        for(unsigned k=0;k<sizes.size();++k)active[k]=&embeddings[k][keys[k]&(sizes[k]-1)];
#ifdef WIKI_EIGHT_WORDS
        if(useOlder)for(unsigned k=0;k<olderActive.size();++k)
            olderActive[k]=&embeddings[2][scramble(olderFolded[k]+0x9e3779b9u*(k+1))&(sizes[2]-1)];
#endif
        for(unsigned d=0;d<dims;++d){
            float s=0;for(auto* e:active)s+=(*e)[d];
#ifdef WIKI_EIGHT_WORDS
            if(useOlder)for(auto* e:olderActive)s+=.5f*(*e)[d];
#endif
#ifdef WIKI_SENTENCE_MEMORY
            s+=shortGain[d]*shortTrace[d]+longGain[d]*longTrace[d];
#endif
            float z=s*.55f;
            hidden[d]=std::clamp(z,-1.f,1.f);derivative[d]=(z>-1.f && z<1.f)?.55f:0.f;
        }
        gradient.fill(0);
    }
    void finish(){
        for(unsigned d=0;d<dims;++d){
            float amount=.018f*std::clamp(gradient[d],-6.f,6.f)*derivative[d];
            for(auto* e:active)(*e)[d]=std::clamp((*e)[d]+amount,-3.f,3.f);
#ifdef WIKI_EIGHT_WORDS
            if(useOlder)for(auto* e:olderActive)(*e)[d]=std::clamp((*e)[d]+.5f*amount,-3.f,3.f);
#endif
#ifdef WIKI_SENTENCE_MEMORY
            shortGain[d]=std::clamp(shortGain[d]+amount*shortTrace[d],-3.f,3.f);
            longGain[d]=std::clamp(longGain[d]+amount*longTrace[d],-3.f,3.f);
#endif
        }
    }
    void startByte(unsigned q){node=q;++epoch;}
    static unsigned leftMass(unsigned mass,unsigned prob){return (mass*prob+probUnit/2)/probUnit;}
    Cache& split(unsigned branch,unsigned mass){
        auto& cached=memo[branch];
        if(cached.epoch!=epoch){
            auto p=tree[node*256+branch].predict(hidden);
            cached={epoch,uint16_t(leftMass(mass,p)),uint16_t(p)};
        }
        return cached;
    }
    unsigned mass(unsigned value){
        unsigned h=1,size=unit;
        for(unsigned mask=128;mask && size;mask>>=1){
            auto left=split(h,size).left;
            bool right=(value&mask)!=0;size=right?size-left:left;h=2*h+right;
        }
        return size;
    }
    void observeByte(unsigned value){
        if(!useTree)return;
        ++trainedBytes;unsigned branch=1,size=unit;
        for(unsigned mask=128;mask;mask>>=1){
            bool right=(value&mask)!=0;auto result=split(branch,size);
            tree[node*256+branch].train(hidden,gradient,result.p,!right);
            size=right?size-result.left:result.left;branch=2*branch+right;
        }
    }
    uint32_t adjustBinary(BinaryStep& step,unsigned q,unsigned symbol,uint32_t base){
        constexpr unsigned scale=1u<<20;
        if(!useResidual || base<128 || base>scale-128)return base;
        auto& head=residual[q*256+symbol];
#ifdef WIKI_NET_WIDTH
        const float bias=head.bias[biasGroup];
#else
        const float bias=head.bias;
#endif
        int logOdds=int(std::clamp((bias+dot(hidden,head.weights))*128.f,-512.f,512.f));
        float odds=tables.odds[512+logOdds],p=float(base)/scale;
        p=p*odds/((1.f-p)+p*odds);
        auto adjusted=std::clamp(uint32_t(p*scale+.5f),1u,scale-1);
        step={&head,float(adjusted)/scale};++binaryDecisions;return adjusted;
    }
    void learnBinary(const BinaryStep& step,bool hit){
        if(!step.head)return;
        auto& head=*step.head;float error=float(hit)-step.p;
#ifdef WIKI_NET_WIDTH
        auto& bias=head.bias[biasGroup];
        bias=std::clamp(bias+.012f*error,-2.f,2.f);
#else
        head.bias=std::clamp(head.bias+.012f*error,-2.f,2.f);
#endif
        for(unsigned d=0;d<dims;++d){
            gradient[d]+=error*head.weights[d];
            head.weights[d]=std::clamp(head.weights[d]+.026f*error*hidden[d],-3.f,3.f);
        }
    }
    // Best-first prefix search gives the tree's top three complete legal tokens.
    // It reads the model before observing the actual token and never trains it.
    std::array<uint32_t,3> bestThree()const{
        struct Item {double upper,prior;unsigned q,branch,mass;};
        auto lower=[](const Item& a,const Item& b){return a.upper<b.upper;};
        std::priority_queue<Item,std::vector<Item>,decltype(lower)> queue(lower);
        queue.push({1.,1.,0,1,unit});
        std::array<std::pair<double,uint32_t>,3> best{{{0.,UINT32_MAX},{0.,UINT32_MAX},{0.,UINT32_MAX}}};
        while(!queue.empty() && queue.top().upper>=best[2].first){
            auto item=queue.top();queue.pop();
            if(item.branch<256){
                auto p=tree[item.q*256+item.branch].predict(hidden);
                unsigned left=leftMass(item.mass,p);
                for(unsigned j=0;j<2;++j){
                    unsigned count=j?item.mass-left:left;
                    if(count)queue.push({item.prior*count/unit,item.prior,item.q,item.branch*2+j,count});
                }
                continue;
            }
            unsigned b=item.branch-256,id=UINT32_MAX,q=0;
            if(!item.q){
                if(b<128)id=65536+b;
                else if(b<208)id=b-128;
                else if(b<=240)q=1+b-208;
                else if(b==248)id=65536+256;
                else if(b==255)q=290;
            }else if(item.q<=32)id=80+256*(item.q-1)+b;
            else if(item.q==33)q=34+b;
            else if(item.q==290){if(b>=128)id=65536+b;}
            else {unsigned v=8272+256*(item.q-34)+b;if(v<65536)id=v;}
            if(q && q<wireNodes){queue.push({item.upper,item.upper,q,1,unit});continue;}
            if(id==UINT32_MAX)continue;
            auto pick=std::make_pair(item.upper,id);
            for(auto& place:best)if(pick.first>place.first || (pick.first==place.first && pick.second<place.second))
                std::swap(pick,place);
        }
        return {best[0].second,best[1].second,best[2].second};
    }
    template<class Ranks> void audit(unsigned id,const Ranks& old){
        ++probes;if(id>=65536)return;
        ++probeWords;bool was=false;for(auto g:old)was|=g.id==id;
        bool hit=std::find(alternative.begin(),alternative.end(),id)!=alternative.end();
        oldHits+=was;treeHits+=hit;probeMisses+=!was;rescued+=!was && hit;
        exactPhraseOnly+=was && !hit;bothHit+=was && hit;
    }
};
template<class Core> struct Forecast {
    const Core& base;Model& net;int32_t& gate;
    unsigned baseFactor,netFactor,total,strength,support;
    Forecast(const Core& c,Model& n,unsigned q,unsigned gap):base(c),net(n),
        gate(n.mixture[(c.strength*4+(!q?0:q<=32?1:q==33?2:3))*4+std::min(gap,3u)]){
        const unsigned share=unsigned(gate>>12);
        baseFactor=c.total?std::max(1u,((1024-share)*2048+c.total/2)/c.total):0;
        netFactor=c.total?std::max(1u,(share*2048+unit/2)/unit):1;
        total=baseFactor*c.total+netFactor*unit;strength=c.strength;support=std::max(64u,c.support);
    }
    unsigned operator()(unsigned c)const{return baseFactor*base(c)+netFactor*net.mass(c);}
    void observe(unsigned c){
        if(!baseFactor)return;
        uint64_t a=netFactor*net.mass(c),b=baseFactor*base(c);
        if(a+b)gate=std::clamp(gate+(int32_t((a*(1024u<<12))/(a+b))-gate)/192,4<<12,1000<<12);
    }
};
}
