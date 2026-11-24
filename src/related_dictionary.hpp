// Encoder only. Keep frequency tiers fixed and cluster words by neighboring anchors.
inline void orderByUsage(View src,Dictionary& dictionary){
    auto& words=dictionary.words;size_t n=words.size();
#ifdef WIKI_GRAMMAR_CLASSES
    usageClasses.resize(n);
    for(size_t i=0;i<n;++i)usageClasses[i]=char(std::min<size_t>(i,255));
#endif
    if(n<=one)return;
    Index ids;ids.reserve(n);
    for(size_t i=0;i<n;++i)ids.emplace(words[i],uint32_t(i));
    constexpr size_t anchors=32,dimensions=anchors*2;
    using Profile=std::array<float,dimensions>;
    std::vector<Profile> profiles(n);
    size_t previous=n;
    for(size_t i=0;i<src.size();){
        if(!letter(uint8_t(src[i]))){if(src[i]=='\n')previous=n;++i;continue;}
        size_t begin=i++;
        while(i<src.size() && letter(uint8_t(src[i])))++i;
        auto found=ids.find(src.substr(begin,i-begin));
        size_t current=found==ids.end()?n:found->second;
        if(previous<anchors && current<n)++profiles[current][previous];
        if(current<anchors && previous<n)++profiles[previous][anchors+current];
        previous=current;
    }
    for(auto& p:profiles){
        double total=0;for(float x:p)total+=x;
        if(total)for(float& x:p)x=float(std::sqrt(x/total));
    }
    std::vector<size_t> order(n);std::iota(order.begin(),order.end(),0);
    auto similarity=[&](size_t a,size_t b){double sum=0;for(size_t j=0;j<dimensions;++j)sum+=double(profiles[a][j])*profiles[b][j];return sum;};
    auto lex=[&](size_t a,size_t b){return words[a]<words[b];};
    std::vector<double> projections(n);
    auto split=[&](auto& self,size_t lo,size_t hi)->void{
        if(hi-lo<=16){std::sort(order.begin()+lo,order.begin()+hi,lex);return;}
        auto farthest=[&](size_t anchor){
            size_t best=order[lo];double low=2;
            for(size_t i=lo;i<hi;++i){size_t w=order[i];double s=similarity(w,anchor);if(s<low || (s==low && lex(w,best))){low=s;best=w;}}
            return best;
        };
        auto left=farthest(order[lo]),right=farthest(left);
        if(lex(right,left))std::swap(right,left);
        for(size_t i=lo;i<hi;++i){auto w=order[i];projections[w]=similarity(w,left)-similarity(w,right);}
        size_t mid=lo+(hi-lo)/2;
        std::nth_element(order.begin()+lo,order.begin()+mid,order.begin()+hi,[&](size_t a,size_t b){
            return projections[a]!=projections[b]?projections[a]>projections[b]:lex(a,b);
        });
        self(self,lo,mid);self(self,mid,hi);
    };
    double before=0,after=0;size_t pairs=0;
    for(size_t lo=one;lo<n;){
        auto hi=std::min(std::min(n,lo+2048),lo<one+two?size_t(one+two):n);
        std::sort(order.begin()+lo,order.begin()+hi,lex);
        for(size_t i=lo+1;i<hi;++i)before+=similarity(order[i-1],order[i]);
        split(split,lo,hi);
        for(size_t i=lo+1;i<hi;++i)after+=similarity(order[i-1],order[i]);
        pairs+=hi-lo-1;lo=hi;
    }
    auto original=words;for(size_t i=one;i<n;++i)words[i]=original[order[i]];
#ifdef WIKI_GRAMMAR_CLASSES
    // Reuse the 64-dimensional context profiles across the whole vocabulary.
    // Word IDs stay fixed; the resulting class labels travel in the archive.
    auto finalOrder=order;std::iota(order.begin(),order.end(),0);split(split,64,n);
    std::vector<uint8_t> label(n);
    for(unsigned k=0;k<64;++k)label[k]=uint8_t(k);
    for(size_t k=64;k<n;++k)label[order[k]]=uint8_t(64+192*(k-64)/(n-64));
    for(size_t i=0;i<n;++i)usageClasses[i]=char(label[finalOrder[i]]);
#endif
    if(pairs)std::cerr<<"{\"dictionary_adjacent_similarity_before\":"<<before/pairs<<",\"dictionary_adjacent_similarity_after\":"<<after/pairs<<",\"dictionary_adjacent_pairs\":"<<pairs<<"}\n";
}
