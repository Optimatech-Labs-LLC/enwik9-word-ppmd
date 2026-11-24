// Applied after the word forecasts and existing binary corrections.
// Adjust the odds of the leading candidate, preserving the relative mass of all alternatives.
struct WordFinal {
    static constexpr unsigned unit=32768,bins=64;
    std::vector<int16_t> residual=std::vector<int16_t>(2049*bins);
    unsigned key=2048;
    uint64_t trained=0,adjusted=0;
    void previous(unsigned word){key=groups::hash(word+1)&2047;}
    template<class Range> struct Partition {
        Range& rc;WordFinal& model;
        uint64_t boundary=0,total=0;
        unsigned p=0,a=128;
        int16_t* cell=nullptr;
        Partition(Range& r,WordFinal& m):rc(r),model(m){}
        void start(uint64_t f,uint64_t t){
            boundary=f;total=t;
            if(!f || f>=t || t>normalizeAt/512)return;
            p=unsigned((__uint128_t(f)*unit+t/2)/t);
            if(p<16 || p>unit-16)return;
            unsigned tail=std::min(p,unit-p),power=31u-unsigned(__builtin_clz(tail));
            unsigned bin=2*power+((tail-(1u<<power))>>(power-1))+(p>unit/2?32:0);
            cell=&model.residual[model.key*bins+bin];++model.trained;
            int reach=int(tail/3);unsigned target=unsigned(int(p)+std::clamp(int(*cell)/2,-reach,reach));
            uint64_t denominator=uint64_t(unit-target)*p;
            a=unsigned((128ull*target*(unit-p)+denominator/2)/denominator);
            if(a!=128)++model.adjusted;
        }
        uint64_t scale(uint64_t x)const{
            return x<=boundary?x*a:boundary*a+(x-boundary)*128;
        }
        uint64_t threshold(uint64_t t){
            if(a==128)return rc.threshold(t);
            auto v=rc.threshold(scale(t)),middle=boundary*a;
            return v<middle?v/a:boundary+(v-middle)/128;
        }
        void commit(uint64_t from,uint64_t width){
            if(a==128)rc.commit(from,width);
            else{auto left=scale(from);rc.commit(left,scale(from+width)-left);}
            if(cell){
                int target=from==0?int(unit):0;
                *cell=int16_t(int(*cell)+(target-int(p)-int(*cell))/256);
            }
        }
    };
};
