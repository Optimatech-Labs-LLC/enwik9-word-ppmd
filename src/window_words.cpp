// Online token forecasts mixed with the short-context PPMd distribution.
#define WIKI_GROUP_LIBRARY
#define WIKI_PAGE_LIBRARY
#define NO_ZSTD
#include "word_groups.cpp"
#include "page_codec.cpp"
#include "window_history.hpp"
#include "window_ppm.hpp"
#include <iomanip>

namespace window {
size_t dictionaryLength(View data){
    Reader in{data};unsigned n=in.integer();check(n<=wordCount,"Word dictionary too large");
    for(unsigned i=0;i<n;++i){unsigned len=in.byte();check(len>=2&&len<=40,"Bad spelling");in.take(len);}
    return in.at;
}
struct Sample {
    uint64_t count=0;
    double base=0,combined=0;
    void add(const Cost& cost){++count;base+=cost.base;combined+=cost.combined;}
};
struct Stats {
    uint64_t events=0,words=0,match16=0,matchFull=0;
    std::array<Sample,4> all{},dictionary{};
    void record(unsigned id,const std::array<Rank,3>& guesses,const Cost& cost) {
        unsigned r=0;while(r<3 && guesses[r].id!=id)++r;
        all[r].add(cost);if(id<wordCount)dictionary[r].add(cost);
    }
    static void list(const std::array<Sample,4>& s){
        std::cerr<<'[';
        for(size_t i=0;i<s.size();++i){if(i)std::cerr<<',';std::cerr<<"{\"n\":"<<s[i].count
            <<",\"ppm_bits\":"<<s[i].base<<",\"mixed_bits\":"<<s[i].combined<<'}';}
        std::cerr<<']';
    }
    void print()const{
        std::cerr<<std::fixed<<std::setprecision(3)<<"{\"token_events\":"<<events<<",\"word_ids\":"<<words
                 <<",\"match_at_least_16\":"<<match16<<",\"match_reached_window\":"<<matchFull
                 <<",\"sample_period\":1024,\"all_by_expert_rank\":";list(all);
        std::cerr<<",\"dictionary_targets_by_expert_rank\":";list(dictionary);std::cerr<<"}\n";
    }
};

template<class Range>
void process(Range& rc,Recipe recipe,View input,Bytes* output,size_t length,size_t dictSize,Stats& stats) {
    check(dictSize<=length && length<=limit,"Invalid word frame length");
    ByteModel ppm;History192 history(recipe);Parser parser;
    std::array<Rank,3> guesses{};Cost cost;bool sample=false;
    for(size_t i=0;i<length;++i){
        int actual=Range::reading?-1:uint8_t(input[i]);
        unsigned decoded;
        if(i<dictSize)decoded=ppm.transfer(rc,actual,nullptr,0,nullptr);
        else {
            if(!parser.node){
                history.begin();
                if constexpr(!Range::reading) {
                    sample=(history.position&1023)==0;
                    if(sample){guesses=history.topThree();cost={};}
                }
            }
            Evidence evidence(history,parser.node);
            decoded=ppm.transfer(rc,actual,&evidence,parser.node,sample?&cost:nullptr);
            auto id=parser.take(decoded);
            if(id!=noToken){
                if constexpr(!Range::reading){
                    if(sample)stats.record(id,guesses,cost);
                    ++stats.events;stats.words+=id<wordCount;
                }
                history.observe(id);
            }
        }
        if constexpr(Range::reading)(*output)[i]=char(decoded);
    }
    check(!parser.node,"Word frame ended inside a token");
    if(history.retrieval){stats.match16=history.retrieval->deepPositions;stats.matchFull=history.retrieval->fullPositions;}
}
#ifndef DECODE_ONLY
Bytes prepare(View source){
    auto pages=wiki_pages::transform(source,false,true,true);
    auto tokens=groups::transform(pages,groups::Mode::multiplyOnly);
    Bytes frame="WPF1";put(frame,uint32_t(source.size()));put(frame,uint32_t(pages.size()));frame+=tokens;
    return frame;
}
Bytes pack(View prepared,unsigned mode) {
    Reader frame{prepared};check(frame.take(4)=="WPF1","Wrong prepared word frame");
    unsigned size=frame.integer(),pageSize=frame.integer();auto data=frame.data.substr(frame.at);
    auto dictSize=dictionaryLength(data);RangeOut coder;Stats stats;
    process(coder,recipes[mode],data,nullptr,data.size(),dictSize,stats);
    Bytes archive="WW192V01";archive.push_back(char(mode));
    put(archive,size);put(archive,pageSize);put(archive,uint32_t(data.size()));put(archive,uint32_t(dictSize));
    archive+=coder.finish();stats.print();return archive;
}
#endif
Bytes unpack(View archive){
    Reader in{archive};check(in.take(8)=="WW192V01","Unknown window archive");unsigned mode=in.byte();
    check(mode<recipes.size(),"Invalid recipe");
    unsigned size=in.integer(),pageSize=in.integer(),n=in.integer(),dictSize=in.integer();
    check(size<=limit && pageSize<=limit && n<=limit,"Window archive too large");
    RangeIn coder(in.data.substr(in.at));Stats stats;Bytes tokens(n,'\0');
    process(coder,recipes[mode],{},&tokens,n,dictSize,stats);coder.finish();
    check(dictionaryLength(tokens)==dictSize,"Dictionary boundary mismatch");
    auto pages=groups::inverse(tokens,pageSize,groups::Mode::multiplyOnly);
    auto out=wiki_pages::restoreFrame(pages);check(out.size()==size,"Original size mismatch");return out;
}
}
#ifndef WIKI_WINDOW_LIBRARY
int main(int argc,char** argv) {
    try{
        check(argc>=4,"Usage: window-words pack MODE INPUT ARCHIVE | restore ARCHIVE OUTPUT");
        View command=argv[1];Bytes out;const char* dest=nullptr;
        if(argc==4 && command=="restore"){out=window::unpack(readFile(argv[2]));dest=argv[3];}
#ifndef DECODE_ONLY
        else if(argc==4 && command=="prepare"){out=window::prepare(readFile(argv[2]));dest=argv[3];}
        else if(argc==5 && (command=="pack" || command=="pack-frame")){
            auto it=std::find_if(window::recipes.begin(),window::recipes.end(),[&](auto r){return r.name==View(argv[2]);});
            check(it!=window::recipes.end(),"Unknown recipe");auto source=readFile(argv[3]);
            if(command=="pack")source=window::prepare(source);
            out=window::pack(source,unsigned(it-window::recipes.begin()));dest=argv[4];
        }
#endif
        else throw std::runtime_error("Invalid arguments");
        if(View(dest)=="-"){std::cout.write(out.data(),out.size());std::cout.flush();check(bool(std::cout),"Output failed");}
        else{std::ofstream f(dest,std::ios::binary);f.write(out.data(),out.size());f.close();check(bool(f),"Output failed");}
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
