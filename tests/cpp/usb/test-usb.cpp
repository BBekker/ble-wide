// SPDX-License-Identifier: MIT
#include "frames.h"
#include "ring.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <sys/wait.h>

static void be16(uint8_t *p,unsigned x){p[0]=x>>8;p[1]=x;}
static void le32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=x>>(8*i);}
static void le64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;++i)p[i]=x>>(8*i);}
static void ip_checksum(std::vector<uint8_t>& p,unsigned ip){
 p[ip+10]=p[ip+11]=0;uint32_t s=usb_rx::checksum_sum(p.data()+ip,(p[ip]&15)*4);
 while(s>>16)s=(s&65535)+(s>>16);be16(p.data()+ip+10,~s);
}
static std::vector<uint8_t> frame(unsigned seq=0,bool vlan=false,bool options=false){
 const unsigned eth=vlan?18:14,ihl=options?24:20,n=2*(16+4096);
 std::vector<uint8_t> p(eth+ihl+8+n+4);
 p[0]=0x80;p[6]=2;be16(p.data()+12,vlan?0x8100:0x0800);
 if(vlan){be16(p.data()+14,123);be16(p.data()+16,0x0800);}
 p[eth]=0x40+ihl/4;be16(p.data()+eth+2,ihl+8+n);p[eth+8]=64;p[eth+9]=17;
 p[eth+12]=10;p[eth+13]=77;p[eth+15]=1;p[eth+16]=10;p[eth+17]=77;p[eth+19]=10;
 ip_checksum(p,eth);
 uint8_t *u=p.data()+eth+ihl;be16(u,7700);be16(u+2,7700+(seq&1));be16(u+4,8+n);
 for(unsigned block=0;block<2;++block){
  uint8_t *b=u+8+block*4112;le32(b,0x31385149);b[6]=0;b[7]=8;le64(b+8,seq*2+block);
  for(unsigned i=0;i<2048;++i){b[16+2*i]=44;b[17+2*i]=uint8_t(-55);}
 }
 return p;
}
static std::vector<uint8_t> packed_frame(unsigned seq){
 auto f=frame(seq);f.resize(14+20+8+16+7168+4);
 be16(f.data()+14+2,20+8+16+7168);ip_checksum(f,14);
 uint8_t *udp=f.data()+14+20;be16(udp+4,8+16+7168);
 uint8_t *b=udp+8;le32(b,0x31524453);b[4]=b[5]=0;b[6]=0;b[7]=28;le64(b+8,seq);
 const uint32_t word=(44u<<16)|(uint32_t(-55)&0x3fff);
 for(unsigned g=0;g<256;++g)for(unsigned k=0;k<7;++k)
  le32(b+16+g*28+k*4,word|((word<<(14-2*k))&0xc000c000u));
 return f;
}
static std::vector<uint8_t> aggregate(const std::vector<uint8_t>& f,uint32_t status=0){
 std::vector<uint8_t> a((16+f.size()+15)&~size_t(15));le32(a.data(),uint32_t(f.size())<<17);le32(a.data()+8,status);
 memcpy(a.data()+16,f.data(),f.size());return a;
}
int main(int argc,char **argv){
 if(argc==3 && (std::string(argv[1])=="--emit" || std::string(argv[1])=="--emit-packed" || std::string(argv[1])=="--emit-batch")){
  usb_rx::TransferRing ring(argv[2],true,getuid());usb_rx::release(ring.header->state,1);
  std::vector<uint8_t> batch;unsigned transfers=0;
  for(unsigned i=0;i<40;++i){
   auto a=aggregate(std::string(argv[1])=="--emit-packed"?packed_frame(i):frame(i));
   batch.insert(batch.end(),a.begin(),a.end());
   if(std::string(argv[1])!="--emit-batch" || i==30 || i==39){while(!ring.push(batch.data(),batch.size()))usleep(1000);batch.clear();++transfers;}
  }
  for(unsigned i=0;i<1000 && usb_rx::acquire(ring.header->tail)<transfers;++i)usleep(1000);
  assert(usb_rx::acquire(ring.header->tail)==transfers);usb_rx::release(ring.header->state,2);return 0;
 }
 usb_rx::Filter filter;
 auto parse=[&](const std::vector<uint8_t>& a){usb_rx::Counters c;usb_rx::aggregate(a.data(),a.size(),filter,c,
  [](const uint8_t *p,size_t n){assert(n==8224);assert(usb_rx::le32(p)==0x31385149);});return c;};
 for(bool vlan:{false,true})for(bool options:{false,true}){
  auto a=aggregate(frame(0,vlan,options));auto c=parse(a);assert(c.datagrams==1 && c.malformed==0);
  auto b=aggregate(frame(1,vlan,options));a.insert(a.end(),b.begin(),b.end());assert(parse(a).datagrams==2);
 }
 // Exercise batches larger than the old 64 KiB limit.
 for(unsigned count:{15u,31u}){
  std::vector<uint8_t> batch;
  for(unsigned i=0;i<count;++i){auto part=aggregate(frame(i));batch.insert(batch.end(),part.begin(),part.end());}
  assert(batch.size()<=usb_rx::slot_bytes && parse(batch).datagrams==count);
 }
 auto tagged=frame(0,true);tagged.insert(tagged.begin()+14,{0,1,0x81,0});be16(tagged.data()+12,0x88a8);
 assert(parse(aggregate(tagged)).datagrams==1);
 tagged.insert(tagged.begin()+14,{0,2,0x81,0});assert(parse(aggregate(tagged)).malformed==1);
 auto f=frame();auto a=aggregate(f);
 for(size_t n=1;n<16+f.size();++n){auto short_a=a;short_a.resize(n);assert(parse(short_a).datagrams==0);}
 auto bad=f;bad[14+10]^=1;assert(parse(aggregate(bad)).checksum_errors==1);
 bad=f;be16(bad.data()+14+6,0x2000);ip_checksum(bad,14);assert(parse(aggregate(bad)).fragments==1);
 bad=f;be16(bad.data()+14+20+4,7);assert(parse(aggregate(bad)).malformed==1);
 assert(parse(aggregate(f,1u<<25)).checksum_errors==1);
 assert(parse(aggregate(f,1u<<26)).checksum_errors==1);
 bad=f;bad[14+19]=11;ip_checksum(bad,14);assert(parse(aggregate(bad)).ignored==1);
 // Verify nonzero UDP checksums when no hardware validation is available.
 bad=f;uint8_t *ip=bad.data()+14,*udp=ip+20;unsigned len=usb_rx::be16(udp+4);
 uint32_t sum=usb_rx::checksum_sum(udp,len,usb_rx::checksum_sum(ip+12,8,17+len));
 while(sum>>16)sum=(sum&65535)+(sum>>16);be16(udp+6,(~sum&65535)?~sum:65535);
 assert(parse(aggregate(bad)).datagrams==1);udp[15]^=1;assert(parse(aggregate(bad)).checksum_errors==1);
 // The two-tag path and all malformed lengths get exercised under ASan too.
 std::mt19937 random(17);
 for(unsigned trial=0;trial<10000;++trial){std::vector<uint8_t> v(random()%500);for(auto &x:v)x=random();
  usb_rx::Counters c;usb_rx::aggregate(v.data(),v.size(),filter,c,[](const uint8_t *,size_t){});}
 char tmp[]="/tmp/ble-usb-ring-test-XXXXXX";int fd=mkstemp(tmp);assert(fd>=0);close(fd);unlink(tmp);
 {
  usb_rx::TransferRing reader(tmp,false,getuid());
  usb_rx::TransferRing writer(tmp,true,getuid());
  for(unsigned i=0;i<usb_rx::slot_count;++i){uint8_t b=uint8_t(i);assert(writer.push(&b,1));}
  pid_t child=fork();assert(child>=0);
  if(!child){try{usb_rx::TransferRing duplicate(tmp,true,getuid());_exit(1);}catch(const std::exception& e){_exit(std::string(e.what()).find("already has")!=std::string::npos?0:2);}}
  int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
  uint8_t b=42;assert(!writer.push(&b,1));assert(usb_rx::acquire(writer.header->dropped)==1);
  for(unsigned i=0;i<usb_rx::slot_count;++i){auto s=reader.front();assert(s && s->length==1 && s->data[0]==uint8_t(i));reader.pop();}
  std::vector<uint8_t> full(usb_rx::slot_bytes,0xa5);
  assert(!writer.push(full.data(),full.size()+1));
  assert(writer.push(full.data(),full.size()));
  assert(reader.front()->length==full.size() && !memcmp(reader.front()->data,full.data(),full.size()));reader.pop();
  assert(!reader.front());assert(writer.push(&b,1));assert(reader.front()->data[0]==42);reader.pop();
 }
 unlink(tmp);
 puts("USB aggregates, jumbo IQ8, VLAN/options, checksum/fragment rejection, truncation/fuzz and ring backpressure passed");
}
