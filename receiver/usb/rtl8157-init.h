// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2024 Realtek Semiconductor Corp. All rights reserved.
// Port of Realtek r8152 v2.22.1 RTL_VER_16 initialization; see import-rtl8157.py.
// IO supplies checked field/write/block operations and cancellable millisecond sleep.
// No EEPROM/flash programming: all firmware below is loaded into volatile RAM.
#pragma once
#include "rtl8157-data.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace rtl8157 {
template<class IO> class Initializer {
    IO &io;
    static constexpr uint16_t USB=0, PLA=0x100;
    uint32_t r(uint16_t bus,uint16_t a,unsigned n=2) { return io.field(bus,a,n); }
    void w(uint16_t bus,uint16_t a,uint32_t v,unsigned n=2) { io.write(bus,a,n,v); }
    void m(uint16_t bus,uint16_t a,uint32_t clear,uint32_t set,unsigned n=2) {
        w(bus,a,(r(bus,a,n)&~clear)|set,n);
    }
    template<class F> void wait(const char *stage,unsigned attempts,unsigned ms,F ready) {
        for(unsigned k=0;k<attempts;++k) {
            if(ready())return;
            io.pause(ms);
        }
        throw std::runtime_error(std::string("RTL8157 timeout: ")+stage);
    }
    // PHY access via the USB TGPHY mailbox, as used by Realtek's RTL8157 driver.
    // Firmware speed-up writes can change the legacy OCP page underneath the host.
    void phy_ready() { wait("PHY mailbox",10,2,[&]{return !(r(USB,0xd634)&1);}); }
    uint16_t phy(uint16_t a) {
        phy_ready();w(USB,0xd630,a);w(USB,0xd634,1);phy_ready();
        return uint16_t(r(USB,0xd632));
    }
    void phy(uint16_t a,uint16_t v) {
        phy_ready();w(USB,0xd632,v);w(USB,0xd630,a);w(USB,0xd634,3);
    }
    void pm(uint16_t a,uint16_t clear,uint16_t set) { phy(a,(phy(a)&~clear)|set); }
    uint16_t sram(uint16_t a,bool second=false) {
        phy(second?0xb87c:0xa436,a);return phy(second?0xb87e:0xa438);
    }
    void sm(uint16_t a,uint16_t clear,uint16_t set,bool second=false) {
        uint16_t value=(sram(a,second)&~clear)|set;
        phy(second?0xb87c:0xa436,a);phy(second?0xb87e:0xa438,value);
    }
    uint16_t phy_status(unsigned desired=0) {
        uint16_t state=0;
        wait("PHY state",500,20,[&]{state=phy(0xa420)&7;
            return desired?state==desired:state==2 || state==3 || state==5;});
        return state;
    }
    void flash_ready() {
        if((r(PLA,0xd3ae)&2) && !(r(USB,0xd284)&0x20))
            wait("flash autoload",100,2,[&]{return r(USB,0xd284)&4;});
    }
    void adv_ready() {wait("advanced mailbox",10,2,[&]{return !(r(USB,0xd5dc)&1);});}
    void advanced_modify(uint16_t command,uint16_t address,uint32_t clear,uint32_t set) {
        adv_ready();w(USB,0xd5d6,address);w(USB,0xd5dc,command|1);adv_ready();
        uint32_t value=(r(USB,0xd5d8,4)&~clear)|set;
        w(USB,0xd5d8,value,4);w(USB,0xd5d6,address);w(USB,0xd5dc,command|3);adv_ready();
    }
    void patch_request(bool enable,bool acknowledge=true) {
        pm(0xb820,0x10,enable?0x10:0);
        if(acknowledge)wait(enable?"PHY patch ready":"PHY patch release",5000,1,
            [&]{return bool(phy(0xb800)&0x40)==enable;});
    }
    void clear_breakpoints(uint16_t bus) {
        w(bus,0xfc48,0);
        const uint8_t zero[32]={};io.block(bus,0xfc28,zero,sizeof(zero));
        io.pause(3);w(bus,0xfc26,0);
    }
    void mac_firmware() {
        // An outside firmware loader invalidates the internal version tags.
        if(r(USB,0xcd54)) {w(USB,0xcd54,0);w(USB,0xcfd6,0);}
        unsigned usb=r(USB,0xcfd7,1),pla=r(USB,0xcfd6,1);
        std::printf("RTL8157 MAC patches: USB=%u PLA=%u (bundled 3/1)\n",usb,pla);
        if(usb<3) {
            clear_breakpoints(USB);io.block(USB,0xe600,usb_patch_16,sizeof(usb_patch_16));
            const uint16_t bp[]={0x337e,0x192a,0x269a,0xc632,0xa108,0x08f4,0x5be6,0x50b2,
                0,0,0x453e,0,0xd092,0x04e4,0xa93e,0x5074};
            w(USB,0xfc26,0xf000);
            for(unsigned i=0;i<16;++i)w(USB,uint16_t(0xfc28+2*i),bp[i]);
            w(USB,0xfc48,0x74ff);w(USB,0xcfd7,3,1);
        }
        if(pla<1) {
            clear_breakpoints(PLA);io.block(PLA,0xf800,pla_patch_16,sizeof(pla_patch_16));
            w(PLA,0xfc26,0x8000);w(PLA,0xfc28,0x1e7e);w(PLA,0xfc38,1);w(USB,0xcfd6,1,1);
        }
    }
    void phy_firmware(bool ext_init) {
        uint16_t version=sram(0x801e);
        std::printf("RTL8157 PHY firmware: installed=%04x bundled=%04x\n",version,phy_version);
        if(version>=phy_version)return;
        if(r(PLA,0xd3ae)&2)m(USB,0xd284,0,0x20); // bypass flash for host-loaded patch
        try {
            patch_request(true,!ext_init);
            for(size_t offset=0;offset<sizeof(ram16);offset+=2048) {
                const size_t n=std::min(size_t(2048),sizeof(ram16)-offset);
                m(USB,0xd284,0,0x44); // GPHY_PATCH_DONE | BACKUP_RESTORE
                io.block(USB,0x9a00,ram16+offset,n);
                m(PLA,0xdc6a,0,0x10);
                wait("PHY firmware chunk",1000,1,[&]{return !(r(PLA,0xdc6a)&0x10);});
            }
            patch_request(false,!ext_init);
        } catch(...) {
            // Best effort release, preserving the original USB/timeout error.
            try {patch_request(false,false);} catch(...) {}
            throw;
        }
        version=sram(0x801e);
        if(version!=phy_version)throw std::runtime_error("RTL8157 PHY firmware version verification failed");
        std::printf("RTL8157 PHY firmware uploaded and verified: %04x (%zu bytes)\n",version,sizeof(ram16));
    }
    void aldps_off() {
        pm(0xa430,4,0);
        wait("ALDPS exit",20,2,[&]{return r(PLA,0xe000)&0x100;});
    }
    void phy_config() {
        flash_ready();mac_firmware();m(USB,0xd81a,1,0);
        const bool ext_init=phy_status()==2;
        phy_firmware(ext_init);
        if(ext_init){pm(0xa466,1,0);pm(0xa468,0x0a,0);}
        pm(0xa400,0x800,0);aldps_off();
        // Disable EEE while programming parameters. Leave it disabled for continuous RX.
        m(PLA,0xe040,3,0);pm(0xa432,0x10,0);pm(0xa6d4,3,0);phy(0xa5d0,0);
        phy_status(3);m(PLA,0xe84c,0x40,0);pm(0xa430,0,3);pm(0xa44a,4,0);
        m(PLA,0xd3c8,1,0,1);
        for(const auto &s:calibration) {
            switch(s.bus) {
                case 'W':phy(s.address,s.set);break;
                case 'P':pm(s.address,s.clear,s.set);break;
                case 'S':sm(s.address,s.clear,s.set);break;
                case 'T':sm(s.address,s.clear,s.set,true);break;
            }
        }
        try {
            patch_request(true);m(PLA,0xe0ce,0,1);pm(0xa442,0x3000,0x800);pm(0xa430,0,0x8000);
            patch_request(false);
        } catch(...) {try{patch_request(false,false);}catch(...){}throw;}
        sm(0x8011,0,0x8000); // green Ethernet tuning
        pm(0xa428,0x200,0);pm(0xa5ea,3,0);
        // ALDPS remains disabled while streaming. Advertise pause capability.
        pm(0xa408,0,0xc00);
    }
    void prepare_mac() {
        m(USB,0xcfd8,1,0);aldps_off();m(PLA,0xe85a,0,8);
        w(PLA,0xc0bc,0xff,1);w(PLA,0xe428,0x10);w(PLA,0xd2e8,0);w(PLA,0xd2cc,0,4);
        m(PLA,0xc010,0x0f,0,4);m(PLA,0xe813,0x0c,0,1);
        m(PLA,0xe84f,0x80,0,1);m(PLA,0xc012,8,0);m(PLA,0xe8de,0x4000,0);
        m(PLA,0xc010,0xc0,0); // keep VLAN tags in frames for the host parser
        w(PLA,0xc016,9216);w(PLA,0xe615,160,1);
        w(PLA,0xc0a6,(9216+6*1024)/16);w(PLA,0xc0aa,(9216+14*1024)/16);
        w(PLA,0xe618,8);w(PLA,0xe61a,640);
        m(USB,0xd4b0,3,0,1);m(USB,0xd4b0,0,3,1);
        m(PLA,0xc0a2,0xfff,8);m(PLA,0xe0cc,0x4000,0);
        m(USB,0xd32a,0x300,0);w(USB,0xd40c,0x00600400,4);
        m(USB,0xd378,0,0x80,1);m(USB,0xcd06,0x8000,0);
    }
public:
    explicit Initializer(IO &transport):io(transport){}
    void validate() {
        const auto revision=r(PLA,0xe612)&0x7cf0;
        if(revision!=0x1030)throw std::runtime_error("RTL8157 initialization requires silicon version 0x1030 (RTL_VER_16)");
    }
    void diagnostics() {
        validate();
        std::printf("RTL8157 PHY: state=%04x BMCR=%04x firmware=%04x USBpatch=%02x PLApatch=%02x\n",
            phy(0xa420),phy(0xa400),sram(0x801e),unsigned(r(USB,0xcfd7,1)),unsigned(r(USB,0xcfd6,1)));
    }
    void power_down_for_test() {
        validate();pm(0xa400,0,0x800);
        wait("test PHY power-down",500,20,[&]{return !(r(PLA,0xe908)&2);});
        std::puts("RTL8157 test: PHY powered down; link confirmed down before initialization");
    }
    void initialize(unsigned link_seconds=30) {
        validate();std::puts("RTL8157 initializing USB, MAC and PHY");
        m(USB,0xcffe,0,8,1);m(USB,0xd3ca,1,0,1);m(USB,0xd26b,1,0,1);
        m(USB,0xcfee,0,0x20);m(USB,0xcfd8,1,0);
        wait("autoload",500,20,[&]{return r(PLA,0xe004)&2;});flash_ready();
        if(phy_status()==2){pm(0xa468,0x0a,0);pm(0xa466,1,0);}
        pm(0xa400,0x800,0);phy_status(3);
        advanced_modify(4,0xc2c0,1u<<29,0); // disable U2/P3
        m(USB,0xcf04,0x87,0,1);m(USB,0xd334,8,0); // interrupt mitigation / auto speed-up
        w(USB,0xcbfc,0x0fff);w(USB,0xd4da,500);
        m(USB,0xd80a,0x31,0);m(USB,0xd81a,1,0);m(USB,0xcfff,3,0,1);
        m(PLA,0xd38c,3,0,1);m(PLA,0xd38a,1,0,1);m(PLA,0xd398,0x100,0);
        w(PLA,0xe81c,0xc0,1);m(PLA,0xe820,8,0);w(PLA,0xe81c,0,1);
        m(PLA,0xe0c8,1,0,1);m(USB,0xcd06,0x800,0);
        m(PLA,0xe0ca,0x8000,0); // keep MAC clock awake
        m(PLA,0xd398,0x8000,(r(PLA,0xe908)&2)?0x8001:1);
        m(USB,0xd406,0xc10,0); // aggregation, 16-byte RX descriptor, no end marker
        advanced_modify(0,0x2300,8,0); // no zero-length RX records
        m(USB,0xd4ae,2,0,1);pm(0xa5b4,0x8000,0);
        phy_config();prepare_mac();
        // Full duplex 10/100/1000/2500/5000 autonegotiation; do not advertise 10G.
        pm(0xa408,0x1e0,0x140);pm(0xa412,0x300,0x200);pm(0xa5d4,0x1180,0x180);
        phy(0xa400,0x9200); // BMCR_RESET | ANENABLE | ANRESTART
        wait("PHY reset",50,20,[&]{return !(phy(0xa400)&0x8000);});
        wait("Ethernet link negotiation",link_seconds*10,100,[&]{return r(PLA,0xe908)&2;});
        const unsigned status=r(PLA,0xe908);
        const unsigned speed=(status&0x1000)?5000:(status&0x400)?2500:(status&0x10)?1000:(status&8)?100:10;
        m(PLA,0xe080,2,0); // disable EEE+ for continuous receive
        m(PLA,0xe612,0x308,0x300);m(PLA,0xe0ce,0,0x100); // full-duplex IFG
        m(PLA,0xc010,0x800,0);
        unsigned pause_packets=10,ratio=10000/speed;
        for(unsigned n=ratio;n>1;n>>=1)--pause_packets;
        if(2*ratio>3*(1u<<pause_packets))--pause_packets;
        m(PLA,0xe61e,0xf8,pause_packets<<4);m(PLA,0xe61e,0,8);
        std::printf("RTL8157 initialized: PHY=%04x link=%u Mb/s\n",status,speed);
    }
};
} // namespace rtl8157
