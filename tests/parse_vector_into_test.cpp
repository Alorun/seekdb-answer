#include "common.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace mvdb;
static bool sameBits(float a, float b){ return std::memcmp(&a,&b,4)==0; }
int main(){
  std::mt19937_64 rng(2026);
  std::uniform_int_distribution<int> dimd(1,256);
  std::uniform_real_distribution<double> vald(-1e6,1e6);
  int fails=0;
  for(int it=0; it<20000; ++it){
    int d=dimd(rng);
    std::string lit="[";
    std::vector<float> expect;
    for(int i=0;i<d;i++){
      double x=vald(rng);
      char buf[64]; std::snprintf(buf,sizeof buf,"%.9g",x);
      if(i) lit+=',';
      lit+=buf;
    }
    lit+="]";
    std::vector<float> a; std::string ea;
    bool oka=parseVectorLiteral(lit,a,ea);
    std::vector<float> bufv(d); size_t cnt=0; std::string eb;
    bool okb=parseVectorLiteralInto(lit,bufv.data(),bufv.size(),cnt,eb);
    if(oka!=okb || (oka && (a.size()!=cnt || cnt!=(size_t)d)) || (oka && !ea.empty()) || (okb && !eb.empty())){
      ++fails; if(fails<5) std::printf("META FAIL it=%d d=%d ok=%d/%d cnt=%zu\n",it,d,oka,okb,cnt); continue;
    }
    if(oka){ for(int i=0;i<d;i++){ if(!sameBits(a[i],bufv[i])){ ++fails; if(fails<5) std::printf("VAL FAIL it=%d i=%d %.9g vs %.9g\n",it,i,a[i],bufv[i]); break; } } }
  }
  // dimension mismatch behavior: literal longer/shorter than cap must be counted
  { std::string lit="[1,2,3]"; std::vector<float> buf(2); size_t cnt=0; std::string e;
    bool ok=parseVectorLiteralInto(lit,buf.data(),2,cnt,e);
    if(!ok || cnt!=3){ ++fails; std::printf("DIM LONG FAIL ok=%d cnt=%zu err=%s\n",ok,cnt,e.c_str()); } }
  { std::string lit="[1,2]"; std::vector<float> buf(3); size_t cnt=0; std::string e;
    bool ok=parseVectorLiteralInto(lit,buf.data(),3,cnt,e);
    if(!ok || cnt!=2){ ++fails; std::printf("DIM SHORT FAIL ok=%d cnt=%zu err=%s\n",ok,cnt,e.c_str()); } }
  { std::string lit="[]"; size_t cnt=99; std::string e;
    bool ok=parseVectorLiteralInto(lit,nullptr,0,cnt,e);
    if(!ok || cnt!=0){ ++fails; std::printf("EMPTY FAIL ok=%d cnt=%zu err=%s\n",ok,cnt,e.c_str()); } }
  const char* bad[]={"","[]x","[","]", "[1,]", "[,1]", "[0x10]", "[nan]", "[inf]", "[1e999]", "[1 2]", "[1,,2]", "[1.2.3]", "[1e]", "[--1]", "[1;2]"};
  for(const char* b: bad){ std::vector<float> a; std::string ea; bool oka=parseVectorLiteral(b,a,ea);
    std::vector<float> buf(8); size_t cnt=0; std::string eb; bool okb=parseVectorLiteralInto(b,buf.data(),buf.size(),cnt,eb);
    if(oka!=okb){ ++fails; std::printf("BAD MISMATCH '%s' ok=%d/%d\n",b,oka,okb); } }
  // Value in-place allocation and release
  { Value v=Value::MakeVecRaw(4); float* p=v.mutableVecData(); if(!p){++fails;} else { p[0]=1; p[1]=2; p[2]=3; p[3]=4; }
    Value w=std::move(v); auto s=w.vec(); if(s.size()!=4 || s[2]!=3){++fails; std::printf("RAW VALUE FAIL\n");} }
  std::printf("vector parse check: 20000 random + dims + malformed, failures=%d\n",fails);
  return fails?1:0;
}
