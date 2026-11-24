// Secondary lexical history for the online predictor. The serialized token
// sequence stays intact. The decoder derives every feature from earlier tokens.
#include "function_vocabulary.hpp"

namespace phrase {
struct ContentHistory {
    static constexpr uint32_t initialHash=2166136261u;
    std::array<uint8_t,window::wordCount> roles{};
    std::array<uint32_t,window::wordCount> dictionaryHashes{};
    std::array<uint32_t,common_words::names.size()> commonHashes{};
    uint32_t latest=0,previous=0,literalHash=initialHash;
    unsigned lastFunction=0,earlierFunction=0,sinceFunction=0,literalLength=0;
    bool escaped=false;

    static uint32_t append(uint32_t hash,unsigned c){return (hash^(c|32u))*16777619u;}
    static uint32_t hashWord(View word){
        uint32_t hash=initialHash;
        for(uint8_t c:word)hash=append(hash,c);
        return hash;
    }
    unsigned role(uint32_t hash,size_t length)const{
        if(length>4)return 0;
        for(unsigned i=0;i<commonHashes.size();++i)
            if(commonHashes[i]==hash && common_words::names[i].size()==length)return i+1;
        return 0;
    }
    explicit ContentHistory(View dictionary){
        for(unsigned i=0;i<commonHashes.size();++i)commonHashes[i]=hashWord(common_words::names[i]);
        Reader r{dictionary};unsigned count=r.integer();
        for(unsigned i=0;i<count;++i){
            unsigned id=(i&~7u)+((5*i)&7u);
            auto spelling=r.take(r.byte());auto hash=hashWord(spelling);
            dictionaryHashes[id]=hash;roles[id]=uint8_t(role(hash,spelling.size()));
        }
    }
    void record(uint32_t hash,unsigned function){
        if(function){earlierFunction=lastFunction;lastFunction=function;sinceFunction=0;}
        else{previous=latest;latest=hash;sinceFunction=std::min(sinceFunction+1,15u);}
    }
    void flushLiteral(){
        if(!literalLength)return;
        record(literalHash,role(literalHash,literalLength));literalHash=initialHash;literalLength=0;
    }
    void letter(unsigned c){literalHash=append(literalHash,c);literalLength=std::min(literalLength+1,5u);}
    void observe(unsigned id){
        if(id<window::wordCount){
            flushLiteral();record(dictionaryHashes[id],roles[id]);return;
        }
        unsigned c=id-window::wordCount;
        if(escaped){escaped=false;flushLiteral();return;}
        if(c==digraph::escape){escaped=true;flushLiteral();return;}
        if(c<256 && digraph::alphabet.inverse[c]>=0){
            View pair=digraph::pairs[unsigned(digraph::alphabet.inverse[c])/4];
            letter(uint8_t(pair[0]));letter(uint8_t(pair[1]));return;
        }
        if(groups::letter(c)){letter(c);return;}
        flushLiteral();
    }
    std::array<unsigned,4> keys()const{
        return {latest,previous,combine(latest,previous),(earlierFunction*21+lastFunction)*16+sinceFunction};
    }
};
}
