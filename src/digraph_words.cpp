#define WIKI_SIMILAR_LIBRARY
#define WIKI_NEIGHBORS
#include "similar_words.cpp"
#include "vowel_side.hpp"
#ifdef WIKI_FIELD_SPLIT
#include "field_streams.hpp"
#endif
#ifdef WIKI_NEURAL_ONLY
#include "solo_words.hpp"
#endif

namespace pair_file {
using namespace phrase;
#if defined(WIKI_EXPERIMENT_MAGIC)
constexpr View magic=WIKI_EXPERIMENT_MAGIC;
static_assert(magic.size()==8);
#elif defined(WIKI_GROUPED_WORDS_ONLY)
constexpr View magic="WWRD0256";
#elif defined(WIKI_GROUPED_OUTPUT)
constexpr View magic="WGRP0256";
#elif defined(WIKI_NEURAL_ONLY) && WIKI_NET_WIDTH == 24
constexpr View magic="WSOL0024";
#elif defined(WIKI_NEURAL_ONLY) && WIKI_NET_WIDTH == 48
constexpr View magic="WSOL0048";
#elif defined(WIKI_NEURAL_ONLY)
constexpr View magic="WSOLO001";
#elif WIKI_NET_WIDTH == 24
constexpr View magic="WCON0024";
#elif WIKI_NET_WIDTH == 48
constexpr View magic="WCON0048";
#elif defined(WIKI_SENTENCE_MEMORY)
constexpr View magic="WTRACE01";
#elif defined(WIKI_CONTENT_CONTEXT)
constexpr View magic="WCONTENT";
#elif defined(WIKI_ONLINE_WORD_NET)
constexpr View magic="WNEXT001";
#elif defined(WIKI_WIDE_PPM)
constexpr View magic="WPOOL001";
#elif defined(WIKI_LARGE_WORD_PPM)
constexpr View magic="WDIGRP02";
#else
constexpr View magic="WDIGRP01";
#endif
#ifdef WIKI_NEURAL_ONLY
using solo_words::transferFrame;
#else
template<class Range> void transferFrame(Range& coder,const Mode& mode,View source,Bytes* out,
                                         size_t length,size_t dict,Sampled& stats){
    code(coder,mode,source,out,length,dict,stats);
}
#endif
#ifndef DECODE_ONLY
Bytes pack(Bytes input,View name,bool inspect){
#ifdef WIKI_GAP_ORDER
    if (!inspect) {
        auto slash = name.find('/');
        check(slash != View::npos, "Expected a gap mode suffix");
        View arm = name.substr(slash + 1);
        name = name.substr(0, slash);
        if (!arm.empty() && arm.back() >= '1' && arm.back() <= '8') {
            gap_order::layout.width = unsigned(arm.back() - '0');
            arm.remove_suffix(1);
        }
        if (arm == "forward") gap_order::layout.mode = gap_order::Mode::forward;
        else if (arm == "anchors") gap_order::layout.mode = gap_order::Mode::anchors;
        else if (arm == "phrases") gap_order::layout.mode = gap_order::Mode::phrases;
        else if (arm == "left") gap_order::layout.mode = gap_order::Mode::leftOnly;
        else if (arm == "combined") gap_order::layout.mode = gap_order::Mode::combined;
        else throw std::runtime_error("Unknown gap experiment");
    }
#endif
#ifdef WIKI_WIDE_PPM
    if(inspect)online_words::setup(0,0);
#ifdef WIKI_NEURAL_ONLY
    else if(name=="gradient")online_words::setup(0,solo_words::gradient);
    else if(name=="native-prior")online_words::setup(0,solo_words::nativePrior);
#ifdef WIKI_GROUPED_OUTPUT
    else if(name=="groups")online_words::setup(0,solo_words::grouped);
#endif
    else throw std::runtime_error("Use gradient or native-prior");
#else
    else online_words::parse(name);
#endif
    name="both";
#endif
    Reader frame{input};
#ifdef WIKI_FOLDED_VOCAB
    check(frame.take(4)=="WFC1","Expected a folded frame with its capitalization stream");
#elif defined(WIKI_GRAMMAR_CLASSES)
    check(frame.take(4)=="WGC1","Expected a prepared word-class frame");
#else
    check(frame.take(4)=="WPF1","Invalid prepared frame");
#endif
    auto raw=frame.integer(),pageSize=frame.integer();
#ifdef WIKI_GRAMMAR_CLASSES
    auto classBytes=frame.integer();check(classBytes<=window::wordCount,"Invalid class map size");
    groups::usageClasses=Bytes(frame.take(classBytes));
    std::cerr<<"{\"grammar_class_map_bytes\":"<<classBytes<<"}\n";
#endif
#ifdef WIKI_FOLDED_VOCAB
    Bytes shapes(frame.take(frame.integer()));std::cerr<<"{\"case_stream_bytes\":"<<shapes.size()<<"}\n";
#endif
    View words=frame.data.substr(frame.at);
    size_t directory;
    {auto pages=groups::inverse(words,pageSize,groups::Mode::multiplyOnly);directory=vowels::directory(pages);
#if defined(WIKI_BIT_BUDGET) || defined(WIKI_FIELD_SPLIT) || defined(WIKI_GAP_ORDER)
    text_regions::layout.scan(pages);
#endif
    }
    auto compact=digraph::encodeFrame(words,directory);compact.report();
#if defined(WIKI_BIT_BUDGET) || defined(WIKI_FIELD_SPLIT)
    text_regions::layout.protectedWire=compact.protectedBytes;
#endif
#ifdef WIKI_SPECIALIST_MODEL
    token_specialists::protectedWire=compact.protectedBytes;
#endif
    if(inspect){
        check(digraph::expandFrame(compact.bytes,compact.dictionary,compact.protectedBytes,words.size())==words,
              "Digraph frame failed round trip");
        return {};
    }
    const auto originalWordBytes=uint32_t(words.size());
#ifdef WIKI_GAP_ORDER
    if (gap_order::layout.mode != gap_order::Mode::forward) {
        gap_order::layout.start = gap_order::bodyStart(compact);
        compact.bytes = gap_order::reorder(compact.bytes, gap_order::layout);
        gap_order::session = std::make_unique<gap_order::Session>(gap_order::layout, nullptr);
    }
#endif
    // Only the compact representation is needed while the history grows.
    Bytes().swap(input);
    auto mode=std::find_if(modes.begin(),modes.end(),[name](const auto& m){return m.name==name;});
    check(mode!=modes.end(),"Invalid phrase mode");RangeOut coder;Sampled stats;
#ifdef WIKI_FIELD_SPLIT
    auto fields=field_streams::split(compact);
    transferFrame(coder,*mode,fields.main,nullptr,fields.main.size(),compact.dictionary,stats);
#else
    transferFrame(coder,*mode,compact.bytes,nullptr,compact.bytes.size(),compact.dictionary,stats);
#endif
    Bytes archive(magic);archive.push_back(char(mode-modes.begin()));
#ifdef WIKI_WIDE_PPM
    archive.push_back(char(online_words::pool));archive.push_back(char(online_words::network));
#endif
    put(archive,raw);put(archive,pageSize);put(archive,uint32_t(compact.bytes.size()));
    put(archive,uint32_t(compact.dictionary));put(archive,originalWordBytes);put(archive,uint32_t(compact.protectedBytes));
#ifdef WIKI_GAP_ORDER
    gap_order::layout.write(archive);
    if (gap_order::session) {
        gap_order::session->finish(); gap_order::session->report();
        std::cerr << "{\"gap_directory_bytes\":" << gap_order::layout.directoryBytes
                  << ",\"gap_wire_prefix_bytes\":" << gap_order::layout.start << "}\n";
    }
#endif
#ifdef WIKI_GRAMMAR_CLASSES
    put(archive,uint32_t(groups::usageClasses.size()));archive+=groups::usageClasses;
#endif
#ifdef WIKI_FOLDED_VOCAB
    put(archive,uint32_t(shapes.size()));archive+=shapes;
#endif
#ifdef WIKI_FIELD_SPLIT
    auto main=coder.finish(),spelling=field_streams::compress(fields.spelling),markup=field_streams::compress(fields.markup);
    for(size_t n:{fields.main.size(),fields.spelling.size(),fields.markup.size(),main.size(),spelling.size(),markup.size()})put(archive,uint32_t(n));
    std::cerr<<"{\"field_main_raw\":"<<fields.main.size()<<",\"field_spelling_raw\":"<<fields.spelling.size()
             <<",\"field_markup_raw\":"<<fields.markup.size()<<",\"field_main_bytes\":"<<main.size()
             <<",\"field_spelling_bytes\":"<<spelling.size()<<",\"field_markup_bytes\":"<<markup.size()
             <<",\"field_markup_runs\":"<<fields.spans<<"}\n";
    archive+=main;archive+=spelling;archive+=markup;
#else
    archive+=coder.finish();
#endif
    return archive;
}
#endif
Bytes restore(View archive){
    Reader in{archive};check(in.take(8)==magic,"Wrong consonant-pair archive");
    unsigned mode=in.byte();check(mode<modes.size(),"Unknown phrase mode");
#ifdef WIKI_WIDE_PPM
    auto pool=in.byte(),network=in.byte();online_words::setup(pool,network);
#ifdef WIKI_NEURAL_ONLY
    check(pool==0 && (network==solo_words::gradient || network==solo_words::nativePrior
#ifdef WIKI_GROUPED_OUTPUT
                      || network==solo_words::grouped
#endif
                     ),
          "Invalid neural-only settings");
#endif
    check(modes[mode].name=="both","Unsupported word model for this format");
#endif
    auto raw=in.integer(),pages=in.integer(),n=in.integer(),dict=in.integer(),original=in.integer(),prefix=in.integer();
#ifdef WIKI_GAP_ORDER
    check(n<=limit && dict<=n && prefix<=n-dict,"Invalid gap frame sizes");
    gap_order::layout.read(in,n,dict);
    Bytes restoredBody;
    if (gap_order::layout.mode != gap_order::Mode::forward) {
        check(gap_order::layout.start >= dict + prefix, "Gap starts within the binary header");
        restoredBody.reserve(n - gap_order::layout.start);
        gap_order::session = std::make_unique<gap_order::Session>(gap_order::layout, &restoredBody);
    }
#endif
#ifdef WIKI_GRAMMAR_CLASSES
    auto classBytes=in.integer();check(classBytes<=window::wordCount,"Invalid class map size");
    groups::usageClasses=Bytes(in.take(classBytes));
#endif
#ifdef WIKI_FOLDED_VOCAB
    auto shapes=in.take(in.integer());
#endif
#ifdef WIKI_SPECIALIST_MODEL
    token_specialists::protectedWire=prefix;
#endif
    check(raw<=limit && pages<=limit && original<=limit && n<=limit && dict<=n,"Invalid frame sizes");
#ifdef WIKI_FIELD_SPLIT
    size_t mainN=in.integer(),spellingN=in.integer(),xmlN=in.integer(),mainBytes=in.integer(),spellingBytes=in.integer(),xmlBytes=in.integer();
    check(mainN<=n && dict<=mainN && prefix<=mainN-dict && spellingN<=n && xmlN<=limit,"Invalid separated lengths");
    auto mainStream=in.take(mainBytes),spellingStream=in.take(spellingBytes),xmlStream=in.take(xmlBytes);
    check(in.at==archive.size(),"Trailing field bytes");
    Bytes main(mainN,'\0');RangeIn coder(mainStream);Sampled stats;transferFrame(coder,modes[mode],{},&main,mainN,dict,stats);coder.finish();
    auto spelling=field_streams::decompress(spellingStream,spellingN),markup=field_streams::decompress(xmlStream,xmlN);
    auto compact=field_streams::join(main,spelling,markup,dict+prefix,n);
    Bytes().swap(main);Bytes().swap(spelling);Bytes().swap(markup);
#else
    Bytes compact(n,'\0');RangeIn coder(archive.substr(in.at));Sampled stats;
    transferFrame(coder,modes[mode],{},&compact,n,dict,stats);coder.finish();
#ifdef WIKI_GAP_ORDER
    if (gap_order::session) {
        gap_order::session->finish();
        check(restoredBody.size() == n - gap_order::layout.start, "Gap inverse length mismatch");
        std::copy(restoredBody.begin(), restoredBody.end(), compact.begin() + gap_order::layout.start);
    }
#endif
#endif
    auto words=digraph::expandFrame(compact,dict,prefix,original);
    compact.clear();compact.shrink_to_fit();auto pageData=groups::inverse(words,pages,groups::Mode::multiplyOnly);
#ifdef WIKI_ID_CASE
    Bytes stableDictionary=words.substr(0,dictionaryLength(words));
    Reader known{stableDictionary};unsigned count=known.integer();case_shape::Lexicon selected;
    selected.reserve(count);for(unsigned i=0;i<count;++i)selected.emplace(known.take(known.byte()),i);
#endif
    words.clear();words.shrink_to_fit();
#ifdef WIKI_FOLDED_VOCAB
    RangeIn flags(shapes);
#ifdef WIKI_ID_CASE
    case_shape::transfer(flags,pageData,&selected);
#else
    case_shape::transfer(flags,pageData);
#endif
    flags.finish();
#endif
    auto result=wiki_pages::restoreFrame(pageData);
    check(result.size()==raw,"Incorrect restored length");return result;
}
}
int main(int argc,char** argv){
    try{
        Bytes result;check(argc>=3,"Use pack MODE VOCAB INPUT ARCHIVE or restore ARCHIVE OUTPUT");View op=argv[1];
        if(op=="restore" && argc==4)result=pair_file::restore(readFile(argv[2]));
#ifndef DECODE_ONLY
        else if(op=="pack" && argc==6){
            unsigned cap=unsigned(std::stoul(argv[3]));check(cap && cap<=window::wordCount,"Bad vocabulary size");
            // Finish consuming the source before allocating the entropy model.
            auto prepared=phrase::frame(readFile(argv[4]),cap);
            result=pair_file::pack(std::move(prepared),argv[2],false);
        }else if(op=="pack-frame" && argc==5)result=pair_file::pack(readFile(argv[3]),argv[2],false);
        else if(op=="inspect" && argc==3){pair_file::pack(readFile(argv[2]),"both",true);return 0;}
#endif
        else throw std::runtime_error("Invalid command");
        if(View(argv[argc-1])=="-"){std::cout.write(result.data(),result.size());std::cout.flush();check(bool(std::cout),"Write failed");}
        else wiki_pages::writeFile(argv[argc-1],result);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
