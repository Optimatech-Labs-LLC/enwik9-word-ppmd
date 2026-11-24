// Encoder-only sidecar used for measurement. The decoder never sees this file.
namespace text_regions {
class SourceLedger {
    Bytes path;
    std::ofstream output;
    Bytes packed;
    size_t at=0,end=0,covered=0,pending=0;
    Kind current=other;
    static Bytes readMap(const char* location){
        View name(location);
        if(name.size()<3 || name.substr(name.size()-3)!=".gz")return readFile(location);
        // Only an encoder instrumentation path. The shell reads a quoted
        // environment value as a filename; no filename is inserted into code.
        FILE* input=popen("/usr/bin/gzip -dc < \"$WIKI_READ_LEDGER_MAP\"","r");
        check(input!=nullptr,"Cannot open compressed source ledger");
        Bytes data;std::array<char,65536> chunk{};
        while(size_t n=std::fread(chunk.data(),1,chunk.size(),input)){
            data.append(chunk.data(),n);if(data.size()>limit)break;
        }
        bool valid=!std::ferror(input) && data.size()<=limit;
        int status=pclose(input);check(valid && status==0,"Invalid compressed source ledger");return data;
    }
    void flush(){
        if(!pending)return;
        const auto first=std::min<size_t>(pending,32);
        output.put(char(((first-1)<<3)|unsigned(current)));
        if(first==32){Bytes extra;put(extra,uint32_t(pending-first));output.write(extra.data(),extra.size());}
        pending=0;
    }
    void next(){
        check(at<packed.size(),"Short source attribution map");uint8_t b=uint8_t(packed[at++]);
        current=Kind(b&7);check(current<kinds,"Invalid source category");size_t n=(b>>3)+1;
        if(n==32){Reader r{packed,at};n+=r.integer();at=r.at;}
        end+=n;
    }
public:
    explicit SourceLedger(size_t corpus=layout.end-layout.directory){
        if(corpus!=1000000000)return;
#ifdef WIKI_LEDGER_REFERENCE
        const char* target=std::getenv("WIKI_WRITE_LEDGER_MAP");
        if(target){path=target;output.open(path+".part",std::ios::binary);check(bool(output),"Cannot open source ledger");}
#else
        const char* source=std::getenv("WIKI_READ_LEDGER_MAP");
        if(source)packed=readMap(source);
#endif
    }
    Kind atPosition(size_t pos,Kind fallback=other){
        if(packed.empty())return fallback;
        while(pos>=end)next();
        return current;
    }
    Kind classify(const Mark& mark){
        if(output.is_open() && mark.rawBytes){
            if(pending && current!=mark.kind)flush();
            current=mark.kind;pending+=mark.rawBytes;
        }
        covered+=mark.rawBytes;
        if(packed.empty() || !mark.rawBytes)return mark.kind;
        return atPosition(mark.anchor);
    }
    void finish(){
        if(output.is_open()){
            flush();output.close();check(bool(output) && covered==layout.end,"Source ledger write failed");
            check(std::rename((path+".part").c_str(),path.c_str())==0,"Cannot commit source ledger");
        }
        if(!packed.empty()){
            while(end<covered)next();
            check(covered==layout.end && end==covered && at==packed.size(),"Source ledger does not match frame");
        }
    }
};
}
