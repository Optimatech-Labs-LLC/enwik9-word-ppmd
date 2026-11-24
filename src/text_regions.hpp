// Offline attribution of the existing wire tokens, in reconstructed page coordinates.
#pragma once
namespace text_regions {
using namespace window;
enum Kind { caught, missed, oov, formatting, xml, other, kinds };
struct Layout {
    size_t directory=0,body=0,end=0,protectedWire=0;
    std::vector<std::pair<size_t,size_t>> tags;
    void scan(View pages) {
        tags.clear();Reader r{pages};check(r.take(5)==View("WPG1\3",5),"Unknown page layout");
        size_t n=r.integer(),metadata=0;check(r.integer()==0,"Unexpected page instructions");
        for(size_t i=0;i<n;++i){
            r.integer();size_t total=r.integer();r.integer();size_t content=r.integer();
            check(content<=total,"Invalid body size");metadata+=total-content;
        }
        directory=r.at;body=directory+metadata;end=pages.size();check(body<=end,"Invalid envelope");
        for(size_t i=body;i<end;){
            i=pages.find_first_of("<&",i);if(i==View::npos)break;
            bool escaped=pages.substr(i,4)=="&lt;";size_t j=i+(escaped?4:1);
            if(pages[i]!='<' && !escaped){++i;continue;}
            if(j<end && (pages[j]=='/' || pages[j]=='!' || pages[j]=='?'))++j;
            if(j>=end || !groups::letter(uint8_t(pages[j]))){++i;continue;}
            auto tail=pages.substr(j,std::min<size_t>(4096,end-j));
            View close=escaped?View("&gt;"):View(">");auto at=tail.find(close);
            auto nested=tail.find(escaped?View("&lt;"):View("<"));
            if(at==View::npos || nested<at){++i;continue;}
            tags.emplace_back(i,j+at+close.size());i=tags.back().second;
        }
    }
};
inline Layout layout;
struct Mark { Kind kind;bool wordStart=false,upper=false;size_t rawBytes=0,anchor=0; };
struct Cursor {
    std::vector<View> words;
    size_t pos=0,tag=0;
    unsigned last=0;
    bool flip=false,quote=false,previousOov=false;
    explicit Cursor(View dict){
        Reader r{dict};unsigned n=r.integer();words.resize(n);
        for(auto& word:words)word=r.take(r.byte());
    }
    bool isXml(size_t at){
        if(at<layout.body)return at>=layout.directory;
        while(tag<layout.tags.size() && layout.tags[tag].second<=at)++tag;
        return tag<layout.tags.size() && layout.tags[tag].first<=at;
    }
    Mark take(unsigned id,bool protectedWire,bool outside=false){
        size_t start=pos,where=pos;bool letters=false,upper=false;unsigned c=0;
        if(id<wordCount){
            unsigned original=(id&~7u)|((id*5u)&7);check(original<words.size(),"Bad audit word");
            auto word=words[original];where=pos+(groups::usualSpace(last)!=flip);
            pos=where+word.size();last=uint8_t(word.back());flip=false;
            upper=std::any_of(word.begin(),word.end(),[](char x){return x>='A' && x<='Z';});
        }else if(id==spaceMarker)flip=true;
        else{
            c=id-wordCount;int pair=digraph::alphabet.inverse[c];
            if(!protectedWire && !quote && c==digraph::escape)quote=true;
            else if(!protectedWire && !quote && pair>=0){
                auto word=digraph::pairs[pair/4];last=unsigned(word[1])-((pair&1)?32u:0u);
                pos+=2;letters=true;upper=(pair&3)!=0;
            }else{++pos;last=c;letters=groups::letter(c);upper=c>='A' && c<='Z';quote=false;}
        }
        Kind kind=other;
        if(protectedWire)kind=other;
        else if(isXml(where))kind=xml;
        else if(id<wordCount)kind=outside?missed:caught;
        else if(letters)kind=oov;
        else if(id==spaceMarker || (c>0 && c<128 && (std::isspace(c) || std::ispunct(c))))kind=formatting;
        bool begin=kind==oov && !previousOov;previousOov=kind==oov;
        return {kind,begin,upper,pos-start,where};
    }
    void finish()const{check(pos==layout.end && !quote && !flip,"Field cursor did not consume original page frame");}
};
struct Ledger {
    std::array<double,kinds> bits{};
    std::array<uint64_t,kinds> items{},tokens{},source{},upper{};
    double dictBits=0;
    void add(Mark mark,double cost){
        auto k=mark.kind;bits[k]+=cost;++tokens[k];items[k]+=(k!=oov || mark.wordStart);
        upper[k]+=mark.upper;source[k]+=mark.rawBytes;
    }
    void print()const{
        auto array=[](const auto& a){std::cerr<<'[';for(size_t i=0;i<a.size();++i)std::cerr<<(i?",":"")<<a[i];std::cerr<<']';};
        std::cerr<<std::fixed<<std::setprecision(7)<<"{\"attributed_bits\":";array(bits);
        std::cerr<<",\"attributed_items\":";array(items);std::cerr<<",\"attributed_tokens\":";array(tokens);
        std::cerr<<",\"attributed_source_bytes\":";array(source);std::cerr<<",\"uppercase_bearing_tokens\":";array(upper);
        std::cerr<<",\"dictionary_cost_bits\":"<<dictBits<<",\"envelope_source_bytes\":"<<layout.body-layout.directory
                 <<",\"inline_tag_spans\":"<<layout.tags.size()<<"}\n";
    }
};
}
