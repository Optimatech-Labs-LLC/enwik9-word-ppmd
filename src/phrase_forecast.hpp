// Prior text supplies both the spellings and the continuation counts. Some
// contexts leave a word blank or retain only its spelling class.
namespace phrase {
using namespace window;
uint32_t combine(uint32_t a,uint32_t b) {
    return groups::hash(a*0x9e3779b1u+b+0x85ebca6bu);
}
struct Word { uint32_t folded=0,stem=0,shape=0; };
struct Features {
    std::array<Word,ids> words{};
#ifdef WIKI_EIGHT_WORDS
    std::array<uint32_t,8> lexical{noToken,noToken,noToken,noToken,noToken,noToken,noToken,noToken};
#else
    std::array<uint32_t,3> lexical{noToken,noToken,noToken};
#endif
    unsigned gap=15,gapKind=0;
    explicit Features(View dictionary){
        Reader in{dictionary};unsigned n=in.integer();
        std::unordered_map<Bytes,uint32_t> normalized;
        for(unsigned i=0;i<n;++i){
            unsigned code=(i&~7u)+((5*i)&7u);
            View word=in.take(in.byte());Bytes lower(word);unsigned capitals=0;
            for(char& c:lower)if(c>='A' && c<='Z'){c+=32;++capitals;}
            auto folded=normalized.emplace(lower,code).first->second;
            View stem=lower;
            for(View suffix:{"ing","edly","ed","es","s"}){
                if(stem.size()>=suffix.size()+3 && stem.substr(stem.size()-suffix.size())==suffix){
                    stem.remove_suffix(suffix.size());break;
                }
            }
            uint32_t hash=2166136261u;for(uint8_t c:stem)hash=(hash^c)*16777619u;
            auto tail=uint8_t(lower.back())+32u*uint8_t(lower[lower.size()-2]);
            unsigned form=!capitals?0:capitals==word.size()?1:capitals==1 && word[0]!=lower[0]?2:3;
            words[code]={folded,hash,tail*16+form*4+std::min(3u,unsigned(word.size()/3))};
        }
        for(unsigned id=wordCount;id<ids;++id){
            unsigned c=id-wordCount,folded=c;
            if(c>='0' && c<='9')folded='0';
            if(c>='A' && c<='Z')folded+=32;
            words[id]={wordCount+folded,wordCount+folded,0x100000u+(c>=128 && c<256?128:c)};
        }
    }
    void observe(unsigned id){
#ifdef WIKI_EIGHT_WORDS
        if(id<wordCount){
            std::move_backward(lexical.begin(),lexical.end()-1,lexical.end());
            lexical[0]=id;gap=gapKind=0;return;
        }
#else
        if(id<wordCount){lexical={id,lexical[0],lexical[1]};gap=gapKind=0;return;}
#endif
        gap=std::min(gap+1,15u);
        unsigned c=id-wordCount;
        gapKind=c==256||c==' '?1:c=='\n'?2:groups::letter(c)?3:c>='0'&&c<='9'?4:5;
    }
#ifdef WIKI_STEM_CONTEXTS
    static constexpr unsigned contexts=8;
#else
    static constexpr unsigned contexts=6;
#endif
    struct Query {
        std::array<uint32_t,contexts> key{};
        unsigned used=0,raw=0,gap=0,wide1=0,wide2=0;
    };
    Query query(const History192& h)const{
        Query q;q.gap=gapKind;
        if(!h.position)return q;
        const auto& last=words[h.back(0).token];q.raw=last.folded;
        uint32_t prev=h.position>=2?words[h.back(1).token].folded:ids;
        uint32_t third=h.position>=3?words[h.back(2).token].folded:ids;
        q.key[q.used++]=combine(last.folded,third);
        q.key[q.used++]=combine(last.shape,combine(prev,third));
        q.wide1=combine(last.stem,prev);q.wide2=combine(last.shape,third);
        if(gap>7 || lexical[0]==noToken)return q;
        auto& a=words[lexical[0]];
        const Word missing{ids,ids,ids};
        auto& b=lexical[1]==noToken?missing:words[lexical[1]];
        auto& c=lexical[2]==noToken?missing:words[lexical[2]];
        unsigned separation=std::min(gap,3u)*8+gapKind;
        q.key[q.used++]=combine(a.folded,separation);
        q.key[q.used++]=combine(combine(b.folded,a.stem),separation);
        q.key[q.used++]=combine(combine(c.folded,a.folded),separation);
        q.key[q.used++]=combine(combine(b.folded,a.shape),separation);
#ifdef WIKI_STEM_CONTEXTS
        q.key[q.used++]=combine(a.stem,separation);
        q.key[q.used++]=combine(combine(b.stem,a.stem),separation);
#endif
        q.wide1=combine(a.folded,separation);
        q.wide2=combine(combine(b.folded,a.shape),separation);
        return q;
    }
};

struct WordRow {
    uint32_t tag=0;
    std::array<uint16_t,10> ids{};
    std::array<uint8_t,10> freq{};
    uint16_t total=0;uint8_t used=0;
    void observe(unsigned id){
        if(total==240){
            unsigned n=0;for(unsigned i=0;i<used;++i)if(freq[i]>1){
                ids[n]=ids[i];freq[n++]=freq[i]/2;
            }
            used=uint8_t(n);total/=2;
        }
        ++total;
        if(id>=wordCount)return;
        unsigned least=0;
        for(unsigned i=0;i<used;++i){
            if(ids[i]==id){++freq[i];return;}
            if(freq[i]<freq[least])least=i;
        }
        if(used<ids.size()){ids[used]=uint16_t(id);freq[used++]=1;}
        else if(freq[least]<=2){ids[least]=uint16_t(id);freq[least]=1;}
        else --freq[least];
    }
};
struct Holes {
    static constexpr unsigned slots=1u<<17;
    std::vector<WordRow> rows=std::vector<WordRow>(Features::contexts*slots);
    std::array<WordRow*,Features::contexts> active{};
    unsigned n=0;
    Scratch votes;
    void begin(const Features::Query& q){
        votes.reset();n=q.used;
        for(unsigned i=0;i<n;++i){
            auto key=q.key[i];auto& r=rows[i*slots+(key&(slots-1))];
            if(r.tag!=key){r.tag=key;r.total=r.used=0;}
            active[i]=&r;
            for(unsigned j=0;j<r.used;++j)
                votes.vote(r.ids[j],(i==2?280u:200u)*r.freq[j]/(r.total+10+2*r.used));
        }
    }
    void observe(unsigned id){for(unsigned i=0;i<n;++i)active[i]->observe(id);}
};

// Store the complete next wire-byte histogram, including uncommon continuations.
struct HistogramRow {
    std::array<uint8_t,256> counts{};
    uint16_t total=0;
    void observe(unsigned c){
        if(counts[c]==240 || total>=8192){
            total=0;for(auto& v:counts){v/=2;total+=v;}
        }
        ++counts[c];++total;
    }
};
struct ContextRows {
    struct Entry {HistogramRow row;uint32_t tag=0,waiting=0;};
    std::vector<Entry> entries;
    explicit ContextRows(size_t n):entries(n){}
    HistogramRow* lookup(uint32_t key){
        auto& e=entries[key&(entries.size()-1)];
        if(e.tag==key)return &e.row;
        // A one-off different phrase does not evict a trained context.
        if(e.row.total && e.waiting!=key){e.waiting=key;return nullptr;}
        e.row={};e.tag=key;e.waiting=0;return &e.row;
    }
};
struct Broad {
    std::vector<HistogramRow> first=std::vector<HistogramRow>(ids);
    ContextRows within{1u<<17},lexical{1u<<17},substitute{1u<<17};
    std::array<HistogramRow*,3> current{};
    void begin(const Features::Query& q,unsigned node){
        current[0]=node?within.lookup(combine(q.raw,node)):&first[q.raw];
        current[1]=lexical.lookup(combine(q.wide1,node));
        current[2]=substitute.lookup(combine(q.wide2,node));
    }
    void observe(unsigned c){for(auto* row:current)if(row)row->observe(c);}
};

struct Gain {
    std::array<int32_t,5> mass{520<<12,135<<12,160<<12,120<<12,89<<12};
};
struct Mixer {
    std::array<Gain,7*4*3*3> gates{};
    Gain& select(unsigned strength,unsigned node,unsigned gap,unsigned history){
        unsigned kind=!node?0:node<=32?1:node==33?2:3;
        unsigned sep=gap==0?0:gap==1?1:2;
        return gates[((strength*4+kind)*3+sep)*3+(history<12?0:history<80?1:2)];
    }
};
struct Forecast {
    const Evidence base;
    const Scratch* nearby;
    unsigned node,strength,support=0,total=0;
    std::array<HistogramRow*,3> rows{};
    std::array<unsigned,5> amount{},factor{};
    Gain* gate;
    Forecast(const History192& h,unsigned q,const Holes* holes,const Broad* broad,Gain& g,bool learn):
        base(h,q),nearby(holes?&holes->votes:nullptr),node(q),strength(base.strength),gate(learn?&g:nullptr){
        amount[0]=base.total;
        if(nearby)amount[1]=nearby->get(nearby->total[q]);
        if(broad){rows=broad->current;for(unsigned i=0;i<3;++i)if(rows[i])amount[i+2]=rows[i]->total;}
        for(unsigned i=0;i<5;++i){
            if(!amount[i])continue;
            unsigned prior=unsigned(g.mass[i]>>12);
            if(i>=2)prior=prior*amount[i]/(amount[i]+(i==2?16:8));
            if(!prior)continue;
            factor[i]=std::max(1u,(96*prior+amount[i]/2)/amount[i]);
            total+=amount[i]*factor[i];support=std::max(support,amount[i]);
        }
    }
    unsigned part(unsigned i,unsigned c)const{
        if(!factor[i])return 0;
        if(!i)return factor[i]*base(c);
        if(i==1)return factor[i]*nearby->get(nearby->freq[node*256+c]);
        return factor[i]*rows[i-2]->counts[c];
    }
    unsigned operator()(unsigned c)const{
        return part(0,c)+part(1,c)+part(2,c)+part(3,c)+part(4,c);
    }
    void observe(unsigned c){
        if(!gate)return;
        std::array<unsigned,5> votes{};uint64_t sum=0,active=0;
        for(unsigned i=0;i<5;++i)if(factor[i]){
            votes[i]=part(i,c);sum+=votes[i];active+=gate->mass[i];
        }
        if(!sum)return;
        for(unsigned i=0;i<5;++i)if(factor[i]){
            auto& weight=gate->mass[i];
            int32_t target=int32_t(active*votes[i]/sum);
            weight=std::clamp(weight+(target-weight)/256,12<<12,1600<<12);
        }
    }
};
}
