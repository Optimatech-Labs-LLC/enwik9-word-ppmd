namespace online_words {
constexpr std::array<uint64_t,4> poolBytes{uint64_t(1536)<<20,4000000000ULL,8000000000ULL,16000000000ULL};
constexpr std::array<View,4> poolNames{"1536m","4g","8g","16g"};
inline unsigned pool=0,network=0;
#ifdef WIKI_ONLINE_WORD_NET
constexpr std::array<View,4> predictors{"","-net","-context","-both"};
#else
constexpr std::array<View,1> predictors{""};
#endif
inline void setup(unsigned p,unsigned n){
    check(p<poolBytes.size() && n<predictors.size(),"Unsupported pool or word predictor");
    pool=p;network=n;ppm::memory=size_t(poolBytes[p]);
}
#ifndef DECODE_ONLY
inline void parse(View name){
    for(unsigned i=0;i<poolNames.size();++i){
        for(unsigned j=0;j<predictors.size();++j){
            if(name==Bytes(poolNames[i])+Bytes(predictors[j])){setup(i,j);return;}
        }
    }
    throw std::runtime_error("Unknown pool or predictor");
}
#endif
}
