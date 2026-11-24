#define WIKI_WORD_EXPERTS
#define WIKI_WINDOW_LIBRARY
#ifdef WIKI_ONLINE_WORD_NET
#include "online_word_net.hpp"
#endif
#include "phrase_calibration.hpp"
#include "window_words.cpp"
#include "phrase_forecast.hpp"
#if defined(WIKI_DIGRAPH_FRAME) || defined(WIKI_DIGRAPH_FIRST)
#include "digraph_layout.hpp"
#endif
#if (defined(WIKI_BIT_BUDGET) || defined(WIKI_FIELD_SPLIT) || defined(WIKI_GAP_ORDER)) && !defined(DECODE_ONLY)
#include "text_regions.hpp"
#if defined(WIKI_LEDGER_REFERENCE) || defined(WIKI_FOLDED_VOCAB) || defined(WIKI_NUMERIC_UTF)
#include "source_ledger.hpp"
#endif
#endif
#ifdef WIKI_NEIGHBORS
#include "phrase_neighbors.hpp"
#endif
#ifdef WIKI_CONTENT_CONTEXT
#include "content_history.hpp"
#endif
#ifdef WIKI_WIDE_PPM
#include "neural_settings.hpp"
#endif

#if defined(WIKI_MARKUP_CONTEXT) || defined(WIKI_MISS_GROUPS) || defined(WIKI_NUMERIC_UTF)
#define WIKI_SPECIALIST_MODEL
#include "token_specialists.hpp"
#endif
#ifdef WIKI_GAP_ORDER
#include "gap_order.hpp"
#endif
#ifdef WIKI_FOLDED_VOCAB
#include "case_shape.hpp"
#endif

namespace phrase {
#ifdef WIKI_LARGE_WORD_PPM
constexpr View archiveMagic="WSPHRA02";
#else
constexpr View archiveMagic="WSPHRA01";
#endif
struct Mode {View name;bool holes,wide,adaptive,calibrate;
#ifdef WIKI_NEIGHBORS
    bool match=false,wordCache=false,onlyMisses=false;
#endif
};
#ifdef WIKI_NEIGHBORS
constexpr std::array<Mode,13> modes{{
#else
constexpr std::array<Mode,7> modes{{
#endif
    {"baseline",false,false,false,false},{"holes",true,false,false,false},
    {"broad",false,true,false,false},{"fixed",true,true,false,false},
    {"learned",true,true,true,false},{"calibrated",true,true,true,true},
    {"baseline-cal",false,false,false,true}
#ifdef WIKI_NEIGHBORS
    ,{"near-all",false,false,false,false,true,false,false}
    ,{"near-misses",false,false,false,false,true,false,true}
    ,{"word-cache",false,false,false,false,false,true,true}
    ,{"both",true,true,true,true,true,true,true}
    ,{"mixed-cache",true,true,true,true,false,true,true}
    ,{"mixed-near",true,true,true,true,true,false,true}
#endif
}};
struct Sampled {
    uint64_t events=0,words=0,cases=0,offered=0;
    std::array<uint64_t,4> counts{};
    std::array<double,4> bits{};
#ifdef WIKI_COHORT_AUDIT
    std::array<uint64_t,2> allWordCounts{};
    std::array<double,2> allWordBits{};
#endif
    double offeredBits=0;
    void record(unsigned id,const std::array<Rank,3>& old,double cost,const Holes* holes){
        if(id>=wordCount)return;
        ++cases;unsigned rank=0;while(rank<3 && old[rank].id!=id)++rank;
        ++counts[rank];bits[rank]+=cost;
        if(rank==3 && holes && holes->votes.get(holes->votes.tokens[id])){
            ++offered;offeredBits+=cost;
        }
    }
    void print(const BinaryCalibration* cal
#ifdef WIKI_NEIGHBORS
               ,const Neighbors* neighbors
#endif
    )const{
        std::cerr<<std::fixed<<std::setprecision(6)<<"{\"events\":"<<events<<",\"words\":"<<words
           <<",\"sample_period\":1024,\"sampled_words\":"<<cases<<",\"old_rank_counts\":[";
        for(size_t i=0;i<4;++i)std::cerr<<(i?",":"")<<counts[i];
        std::cerr<<"],\"old_rank_coded_bits\":[";
        for(size_t i=0;i<4;++i)std::cerr<<(i?",":"")<<bits[i];
        std::cerr<<"],\"sampled_old_misses_offered_by_analogies\":"<<offered
             <<",\"offered_bits\":"<<offeredBits<<",\"calibrated_binary_decisions\":"<<(cal?cal->decisions:0);
#ifdef WIKI_NEIGHBORS
        if(neighbors)std::cerr<<",\"all_dict_words_old_top3_missed\":"<<neighbors->misses
            <<",\"sampled_misses_offered_by_inexact_match\":"<<neighbors->sampledOffered
            <<",\"distinct_site_votes\":"<<(neighbors->sites?neighbors->sites->evaluated:0)
            <<",\"correct_site_votes\":"<<(neighbors->sites?neighbors->sites->credited:0);
#endif
#ifdef WIKI_COHORT_AUDIT
        std::cerr<<",\"all_dictionary_word_counts_inside_outside\":["<<allWordCounts[0]<<','<<allWordCounts[1]
                 <<"],\"all_dictionary_word_bits_inside_outside\":["<<allWordBits[0]<<','<<allWordBits[1]<<']';
#endif
        std::cerr<<"}\n";
    }
};
template<class Range> void code(Range& coder,const Mode& mode,View source,Bytes* out,size_t length,size_t dict,Sampled& stats){
    check(dict<=length && length<=limit,"Invalid word frame");
    ByteModel ppm;std::unique_ptr<BinaryCalibration> calibration;
    if(mode.calibrate){calibration=std::make_unique<BinaryCalibration>();ppm.calibration=calibration.get();}
#if defined(WIKI_BIT_BUDGET) && !defined(DECODE_ONLY)
    text_regions::Ledger ledger;
#if defined(WIKI_LEDGER_REFERENCE) || defined(WIKI_FOLDED_VOCAB) || defined(WIKI_NUMERIC_UTF)
    text_regions::SourceLedger reference;
#endif
#endif
    for(size_t i=0;i<dict;++i){
        int actual=-1;if constexpr(!Range::reading)actual=uint8_t(source[i]);
        auto byte=ppm.transfer(coder,actual,nullptr,0,nullptr);
        if constexpr(Range::reading)(*out)[i]=char(byte);
    }
    View spellings=Range::reading?View(*out).substr(0,dict):source.substr(0,dict);
#ifdef WIKI_DIGRAPH_FRAME
    auto originalSpellings=digraph::expandDictionary(spellings);
    spellings=originalSpellings;
    check(dictionaryLength(spellings)==spellings.size(),"Invalid expanded dictionary length");
#else
    check(dictionaryLength(spellings)==dict,"Invalid dictionary length");
#endif
#if defined(WIKI_BIT_BUDGET) && !defined(DECODE_ONLY)
    text_regions::Cursor cursor(spellings);
    if constexpr(!Range::reading)ledger.dictBits=8.*double(coder.bytes.size()+coder.pending-1)+std::log2(double(UINT64_MAX)/double(coder.range));
#endif
    std::unique_ptr<Features> features;std::unique_ptr<Holes> holes;std::unique_ptr<Broad> broad;
    if(mode.holes||mode.wide)features=std::make_unique<Features>(spellings);
    if(mode.holes)holes=std::make_unique<Holes>();
    if(mode.wide)broad=std::make_unique<Broad>();
#ifdef WIKI_NEIGHBORS
    std::unique_ptr<Neighbors> neighbors;
    if(mode.match || mode.wordCache){
        neighbors=std::make_unique<Neighbors>(mode.match,mode.wordCache,mode.onlyMisses);
        if(!features)features=std::make_unique<Features>(spellings);
    }
#endif
#ifdef WIKI_SPECIALIST_MODEL
    token_specialists::Selected specialist;
#endif
    History192 h(recipes[9]);Mixer mixer;Features::Query q;Parser parser;
    std::array<Rank,3> old{};bool sample=false;uint64_t shifts=0,range=0;
#ifdef WIKI_ONLINE_WORD_NET
    std::unique_ptr<word_net::Model> net;
    if(online_words::network){
        check(features && neighbors && calibration,"The neural predictor requires the combined model");
        net=std::make_unique<word_net::Model>(online_words::network);calibration->net=net.get();
    }
    bool probe=false;
#ifdef WIKI_CONTENT_CONTEXT
    ContentHistory content(spellings);
#endif
#endif
#ifdef WIKI_COHORT_AUDIT
    OriginalRank auditRank;
#endif
    for(size_t i=dict;i<length;++i){
        if(!parser.node){
            h.begin();
            if(features)q=features->query(h);
#ifdef WIKI_SPECIALIST_MODEL
            specialist.start(h,*features,q,i>=dict+token_specialists::protectedWire);
#endif
#ifdef WIKI_ONLINE_WORD_NET
            if(net){
                const auto& prior=features->lexical;
                auto folded=[&](unsigned id){return id<ids?features->words[id].folded:ids;};
                unsigned last=h.position?h.back(0).token:ids;
                unsigned shape=prior[0]<ids?features->words[prior[0]].shape:0;
#ifdef WIKI_CONTENT_CONTEXT
                net->contentKeys=content.keys();
#endif
#ifdef WIKI_EIGHT_WORDS
                net->olderWords(*features,ids);
#endif
                net->begin(folded(prior[0]),folded(prior[1]),folded(prior[2]),last,shape,q.gap);
                if constexpr(!Range::reading){
                    probe=net->useTree && (h.position&8191u)==0;
                    if(probe)net->alternative=net->bestThree();
                }
            }
#endif
            if(holes)holes->begin(q);
#ifdef WIKI_NEIGHBORS
            if(neighbors)neighbors->begin(h,*features);
#endif
            if constexpr(!Range::reading){
                sample=(h.position&1023)==0;
                if(sample){old=h.topThree();shifts=coder.bytes.size()+coder.pending;range=coder.range;}
#ifdef WIKI_COHORT_AUDIT
                shifts=coder.bytes.size()+coder.pending;range=coder.range;
#endif
            }
        }
        int actual=-1;if constexpr(!Range::reading)actual=uint8_t(source[i]);
        unsigned byte;
#ifdef WIKI_SPECIALIST_MODEL
        specialist.select(parser.node);
#endif
#ifdef WIKI_GAP_ORDER
        auto* gap = i >= gap_order::layout.start ? gap_order::session.get() : nullptr;
        if (gap) gap->select(parser.node);
#endif
#ifdef WIKI_ONLINE_WORD_NET
        if(net && net->useTree)net->startByte(parser.node);
#endif
#ifdef WIKI_NEIGHBORS
        auto transfer=[&](const auto& core){
            if(neighbors){
                NearbyForecast<std::decay_t<decltype(core)>> prediction(core,*neighbors,parser.node,q.gap);
#ifdef WIKI_ONLINE_WORD_NET
                if(net && net->useTree){
                    word_net::Forecast<std::decay_t<decltype(prediction)>> neural(prediction,*net,parser.node,q.gap);
#ifdef WIKI_GAP_ORDER
                    auto withGap = [&](const auto& base) {
                        if (gap && gap->total) {
                            token_specialists::Blend<std::decay_t<decltype(base)>, gap_order::Session> mixed(base, *gap);
                            auto v = ppm.transfer(coder,actual,&mixed,parser.node,nullptr);
                            mixed.observe(v);
                            return v;
                        }
                        return ppm.transfer(coder,actual,&base,parser.node,nullptr);
                    };
#endif
#ifdef WIKI_SPECIALIST_MODEL
                    if(specialist.total){
                        token_specialists::Blend<std::decay_t<decltype(neural)>,token_specialists::Selected> added(neural,specialist);
#ifdef WIKI_GAP_ORDER
                        auto v=withGap(added);
#else
                        auto v=ppm.transfer(coder,actual,&added,parser.node,nullptr);
#endif
                        added.observe(v);neural.observe(v);prediction.observe(v);return v;
                    }
#endif
#ifdef WIKI_GAP_ORDER
                    auto v=withGap(neural);
#else
                    auto v=ppm.transfer(coder,actual,&neural,parser.node,nullptr);
#endif
                    neural.observe(v);prediction.observe(v);return v;
                }
#endif
                auto v=ppm.transfer(coder,actual,&prediction,parser.node,nullptr);prediction.observe(v);return v;
            }
            return ppm.transfer(coder,actual,&core,parser.node,nullptr);
        };
        if(holes||broad){
#else
        if(features){
#endif
            if(broad)broad->begin(q,parser.node);
            auto& gain=mixer.select(h.retrieval->strength(),parser.node,q.gap,
                                    broad && broad->current[1]?broad->current[1]->total:0);
            Forecast prediction(h,parser.node,holes.get(),broad.get(),gain,mode.adaptive);
#ifdef WIKI_NEIGHBORS
            byte=transfer(prediction);
#else
            byte=ppm.transfer(coder,actual,&prediction,parser.node,nullptr);
#endif
            prediction.observe(byte);if(broad)broad->observe(byte);
        }else{
            Evidence e(h,parser.node);
#ifdef WIKI_NEIGHBORS
            byte=transfer(e);
#else
            byte=ppm.transfer(coder,actual,&e,parser.node,nullptr);
#endif
        }
        if constexpr(Range::reading)(*out)[i]=char(byte);
#ifdef WIKI_ONLINE_WORD_NET
        if(net)net->observeByte(byte);
#endif
        auto id=parser.take(byte);if(id==noToken)continue;
#ifdef WIKI_GAP_ORDER
        if (gap) gap->accept(id);
#endif
#ifdef WIKI_ONLINE_WORD_NET
        if(net){
            if constexpr(!Range::reading)if(probe)net->audit(id,old);
            net->finish();
#ifdef WIKI_CONTENT_CONTEXT
            content.observe(id);
#endif
#ifdef WIKI_SENTENCE_MEMORY
            net->remember(id,features->words[id].folded);
#endif
        }
#endif
        if constexpr(!Range::reading){
            ++stats.events;stats.words+=id<wordCount;
#ifdef WIKI_COHORT_AUDIT
            unsigned outside=id<wordCount && auditRank.missed(h,id);
#ifdef WIKI_MISS_GROUPS
            if(id<wordCount && specialist.active)check(bool(outside)!=specialist.excludedWord(id),"Expert shortlist differs from control");
#endif
#ifdef WIKI_BIT_BUDGET
            auto mark=cursor.take(id,i<dict+text_regions::layout.protectedWire,outside);
#if defined(WIKI_LEDGER_REFERENCE) || defined(WIKI_FOLDED_VOCAB) || defined(WIKI_NUMERIC_UTF)
            mark.kind=reference.classify(mark);
#endif
            ledger.add(mark,8.*double(coder.bytes.size()+coder.pending-shifts)+std::log2(double(range)/double(coder.range)));
#endif
            if(id<wordCount){
                ++stats.allWordCounts[outside];
                stats.allWordBits[outside]+=8.*double(coder.bytes.size()+coder.pending-shifts)+std::log2(double(range)/double(coder.range));
                if(sample)check(bool(outside)==std::none_of(old.begin(),old.end(),[id](auto r){return r.id==id;}),"Rank audit mismatch");
            }
            auditRank.observe(h,id);
#endif
            if(sample){
                double cost=8.*double(coder.bytes.size()+coder.pending-shifts)+std::log2(double(range)/double(coder.range));
                stats.record(id,old,cost,holes.get());
            }
        }
#ifdef WIKI_WORD_FINAL
        if(id<wordCount)ppm.final.previous(features?features->words[id].folded:id);
#endif
#ifdef WIKI_SPECIALIST_MODEL
        specialist.observe(id,h,*features);
#endif
        if(holes)holes->observe(id);
        if(features)features->observe(id);
#ifdef WIKI_NEIGHBORS
        if(neighbors)neighbors->observe(h,id,sample,old);
#endif
        h.observe(id);
    }
    check(!parser.node,"Incomplete word ID");
#ifdef WIKI_SPECIALIST_MODEL
    if constexpr(!Range::reading)std::cerr<<"{\"specialist_decisions\":"<<specialist.predicted<<",\"specialist_observations\":"<<specialist.trained<<"}\n";
#endif
#ifdef WIKI_WORD_FINAL
    if constexpr(!Range::reading)std::cerr<<"{\"word_final_observations\":"<<ppm.final.trained<<",\"word_final_adjustments\":"<<ppm.final.adjusted<<"}\n";
#endif
#if defined(WIKI_BIT_BUDGET) && !defined(DECODE_ONLY)
    if constexpr(!Range::reading){
        cursor.finish();ledger.print();
#if defined(WIKI_LEDGER_REFERENCE) || defined(WIKI_FOLDED_VOCAB) || defined(WIKI_NUMERIC_UTF)
        reference.finish();
#endif
    }
#endif
#ifdef WIKI_ONLINE_WORD_NET
    if constexpr(!Range::reading)if(net)std::cerr<<"{\"neural_hidden_width\":"<<word_net::dims
        <<",\"neural_binary_adjustments\":"<<net->binaryDecisions<<",\"neural_tree_bytes\":"<<net->trainedBytes
        <<",\"probe_period\":8192,\"probe_all_positions\":"<<net->probes<<",\"probe_word_targets\":"<<net->probeWords
        <<",\"probe_old_top3_misses\":"<<net->probeMisses<<",\"probe_old_top3_hits\":"<<net->oldHits
        <<",\"probe_neural_top3_hits\":"<<net->treeHits<<",\"probe_neural_rescues\":"<<net->rescued
        <<",\"probe_only_old_hits\":"<<net->exactPhraseOnly<<",\"probe_both_hit\":"<<net->bothHit<<"}\n";
#endif
#ifdef WIKI_WIDE_PPM
    if constexpr(!Range::reading)std::cerr<<"{\"pool_bytes\":"<<ppm.model.state.Size
        <<",\"ppm_order\":"<<ppm::order<<",\"model_restarts\":"<<ppm.model.state.ModelStarts-1<<"}\n";
#endif
    if constexpr(!Range::reading)stats.print(calibration.get()
#ifdef WIKI_NEIGHBORS
                                            ,neighbors.get()
#endif
    );
}
#ifndef DECODE_ONLY
Bytes frame(View raw,unsigned vocabulary){
    auto pages=wiki_pages::transform(raw,false,true,true);
#ifdef WIKI_FOLDED_VOCAB
    RangeOut flags;
#ifdef WIKI_ID_CASE
    Bytes folded=pages;
    for(size_t i=case_shape::directory(folded);i<folded.size();++i)folded[i]=char(case_shape::lower(uint8_t(folded[i])));
    auto dictionary=groups::build(folded,vocabulary);
    case_shape::transfer(flags,pages,&dictionary.ids);
#else
    case_shape::transfer(flags,pages);
#endif
    auto side=flags.finish();
    Bytes result="WFC1";put(result,uint32_t(raw.size()));put(result,uint32_t(pages.size()));
    put(result,uint32_t(side.size()));result+=side;
#ifdef WIKI_ID_CASE
    result+=groups::transformWithDictionary(pages,groups::Mode::multiplyOnly,dictionary);return result;
#else
    result+=groups::transform(pages,groups::Mode::multiplyOnly,vocabulary);return result;
#endif
#else
    auto words=groups::transform(pages,groups::Mode::multiplyOnly,vocabulary);
#ifdef WIKI_GRAMMAR_CLASSES
    Bytes result="WGC1";put(result,uint32_t(raw.size()));put(result,uint32_t(pages.size()));
    put(result,uint32_t(groups::usageClasses.size()));result+=groups::usageClasses;result+=words;return result;
#else
    Bytes result="WPF1";put(result,uint32_t(raw.size()));put(result,uint32_t(pages.size()));result+=words;return result;
#endif
#endif
}
Bytes encode(View frame,View name){
    Reader input{frame};check(input.take(4)=="WPF1","Invalid prepared input");
    auto n=input.integer(),pages=input.integer();View words=input.data.substr(input.at);
    auto dict=dictionaryLength(words);auto mode=std::find_if(modes.begin(),modes.end(),[name](const auto& m){return m.name==name;});
    check(mode!=modes.end(),"Invalid phrase mode");RangeOut coder;Sampled stats;
    code(coder,*mode,words,nullptr,words.size(),dict,stats);
    Bytes packed(archiveMagic);packed.push_back(char(mode-modes.begin()));
    put(packed,n);put(packed,pages);put(packed,uint32_t(words.size()));put(packed,uint32_t(dict));
    packed+=coder.finish();return packed;
}
#endif
Bytes decode(View archive){
    Reader in{archive};check(in.take(8)==archiveMagic,"Invalid phrase archive");
    unsigned mode=in.byte();check(mode<modes.size(),"Unknown phrase mode");
    auto raw=in.integer(),pages=in.integer(),n=in.integer(),dict=in.integer();
    check(raw<=limit && pages<=limit && n<=limit && dict<=n,"Invalid phrase lengths");
    RangeIn coder(archive.substr(in.at));Bytes words(n,'\0');Sampled stats;
    code(coder,modes[mode],{},&words,n,dict,stats);coder.finish();
    auto framed=groups::inverse(words,pages,groups::Mode::multiplyOnly);
    auto result=wiki_pages::restoreFrame(framed);check(result.size()==raw,"Incorrect restored length");return result;
}
}
#ifndef WIKI_SIMILAR_LIBRARY
int main(int argc,char** argv){
    try{
        check(argc>=4,"Use restore ARCHIVE OUTPUT or pack MODE VOCAB INPUT ARCHIVE");
        View op=argv[1];Bytes out;
        if(op=="restore" && argc==4)out=phrase::decode(readFile(argv[2]));
#ifndef DECODE_ONLY
        else if(op=="pack-frame" && argc==5)out=phrase::encode(readFile(argv[3]),argv[2]);
        else if(op=="pack" && argc==6){
            unsigned cap=unsigned(std::stoul(argv[3]));check(cap && cap<=window::wordCount,"Invalid vocabulary limit");
            out=phrase::encode(phrase::frame(readFile(argv[4]),cap),argv[2]);
        }
#endif
        else throw std::runtime_error("Unknown command");
        if(View(argv[argc-1])=="-"){
            std::cout.write(out.data(),out.size());std::cout.flush();check(bool(std::cout),"Incomplete output");
        }else wiki_pages::writeFile(argv[argc-1],out);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#endif
