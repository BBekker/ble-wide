// SPDX-License-Identifier: GPL-2.0-only
// Simulated USB/TGPHY hardware, including acknowledgements and failure injection.
#include "rtl8157-init.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>

struct Fake {
    std::map<unsigned,uint32_t> registers;
    std::map<uint16_t,uint16_t> phy,sram,sram2;
    uint16_t pointer=0,pointer2=0;
    unsigned writes=0,phy_chunks=0,mac_uploads=0,reset_count=0;
    unsigned firmware_bytes=0,fail_chunk=0;
    uint64_t elapsed=0;
    bool stuck_mailbox=false,stuck_ack=false,stuck_upload=false,no_link=false,bad_version=false;
    bool patching=false;
    Fake() {
        registers[0x100e612]=0x1030;registers[0x100e004]=2;
        registers[0x100e000]=0x100;registers[0x100e908]=0;
        phy[0xa420]=5;phy[0xa400]=0x800;
    }
    static unsigned key(uint16_t bus,uint16_t a) {return (unsigned(bus)<<16)|a;}
    uint32_t field(uint16_t bus,uint16_t a,unsigned) {
        if(a==0xd634 && stuck_mailbox)return 1;
        return registers[key(bus,a)];
    }
    uint16_t phy_read(uint16_t a) {
        if(a==0xa438)return sram[pointer];
        if(a==0xb87e)return sram2[pointer2];
        return phy[a];
    }
    void phy_write(uint16_t a,uint16_t v) {
        phy[a]=v;
        if(a==0xa436)pointer=v;
        if(a==0xa438)sram[pointer++]=v;
        if(a==0xb87c)pointer2=v;
        if(a==0xb87e)sram2[pointer2++]=v;
        if(a==0xb820) {
            patching=v&0x10;
            if(!stuck_ack)phy[0xb800]=patching?0x40:0;
        }
        if(a==0xa400) {
            phy[0xa420]=(v&0x800)?5:3;
            if(v&0x800)registers[0x100e908]=0;
            if(v&0x8000) {
                assert(!patching);
                ++reset_count;phy[a]&=~0x8000;
                if(!no_link)registers[0x100e908]=0x1103;
            }
        }
    }
    void write(uint16_t bus,uint16_t a,unsigned,uint32_t v) {
        ++writes;registers[key(bus,a)]=v;
        if(bus==0 && a==0xd634) {
            const uint16_t address=registers[0xd630];
            if(v&2)phy_write(address,registers[0xd632]);
            else registers[0xd632]=phy_read(address);
            registers[0xd634]=0;
        }
        if(bus==0 && a==0xd5dc)registers[a]=0;
        if(bus==0x100 && a==0xdc6a && (v&0x10) && !stuck_upload) {
            registers[key(bus,a)]&=~0x10;
            if(firmware_bytes==sizeof(rtl8157::ram16) && !bad_version)sram[0x801e]=rtl8157::phy_version;
        }
    }
    void block(uint16_t bus,uint16_t a,const uint8_t *,size_t n) {
        if(bus==0 && a==0x9a00) {
            assert(patching);assert(n<=2048 && !(n&3));
            ++phy_chunks;
            if(phy_chunks==fail_chunk)throw std::runtime_error("injected USB transfer failure");
            firmware_bytes+=n;
        } else if(a==0xe600 || a==0xf800)++mac_uploads;
    }
    void pause(unsigned ms) {elapsed+=ms;}
};

template<class F> void fails(F f,const char *message) {
    bool caught=false;
    try{f();}catch(const std::runtime_error &e){caught=true;assert(std::string(e.what()).find(message)!=std::string::npos);}
    assert(caught);
}
int main() {
    { // Full bring-up from a powered-down PHY, then repeat without redundant uploads.
        Fake io;rtl8157::Initializer<Fake> init(io);init.initialize(1);
        assert(io.phy_chunks==5 && io.mac_uploads==2 && io.reset_count==1);
        assert(io.sram[0x801e]==0x14 && io.phy[0xa420]==3);
        assert(io.phy[0xa5d4]==0x180); // 2.5G/5G, not 10G
        assert(io.registers[0x100c016]==9216);
        assert(!io.patching);init.initialize(1);
        assert(io.phy_chunks==5 && io.mac_uploads==2 && io.reset_count==2);
        init.power_down_for_test();assert(io.registers[0x100e908]==0);
        init.initialize(1);assert(io.registers[0x100e908]&2);
    }
    { // Newer installed firmware must not be downgraded.
        Fake io;io.sram[0x801e]=0x99;io.registers[0xcfd7]=9;io.registers[0xcfd6]=9;
        rtl8157::Initializer<Fake>(io).initialize(1);assert(!io.phy_chunks && !io.mac_uploads);
    }
    { // Unknown silicon is rejected before any register writes.
        Fake io;io.registers[0x100e612]=0x2020;
        fails([&]{rtl8157::Initializer<Fake>(io).initialize(1);},"silicon version");assert(!io.writes);
    }
    {
        Fake io;io.stuck_mailbox=true;
        fails([&]{rtl8157::Initializer<Fake>(io).initialize(1);},"PHY mailbox");
        assert(!io.phy_chunks && !io.reset_count && io.elapsed<=20);
    }
    {
        Fake io;io.stuck_ack=true;
        fails([&]{rtl8157::Initializer<Fake>(io).initialize(1);},"PHY patch ready");
        assert(!io.patching && !io.phy_chunks && !io.reset_count);
    }
    for(unsigned chunk=1;chunk<=5;++chunk) {
        Fake io;io.fail_chunk=chunk;
        fails([&]{rtl8157::Initializer<Fake>(io).initialize(1);},"injected USB");
        assert(!io.patching && !io.reset_count && io.phy_chunks==chunk);
    }
    {
        Fake io;io.stuck_upload=true;
        fails([&]{rtl8157::Initializer<Fake>(io).initialize(1);},"PHY firmware chunk");
        assert(!io.patching && !io.reset_count && io.phy_chunks==1);
    }
    {
        Fake io;io.bad_version=true;
        fails([&]{rtl8157::Initializer<Fake>(io).initialize(1);},"version verification");
        assert(!io.patching && !io.reset_count);
    }
    {
        Fake io;io.no_link=true;
        fails([&]{rtl8157::Initializer<Fake>(io).initialize(1);},"link negotiation");
        assert(!io.patching && io.reset_count==1 && io.elapsed>=1000);
    }
    std::puts("RTL8157 initialization tests passed");
}
