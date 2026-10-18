// SPDX-License-Identifier: GPL-2.0-only
// Register/enable sequence derived from Linux r8152.c:
// Copyright (c) 2014 Realtek Semiconductor Corp. All rights reserved.
// RTL8157 warm receive setup follows the register interface and enable sequence
// in Linux drivers/net/usb/r8152.c (Realtek and Linux contributors).
// IOUSBHost transport, bounded capture, and IPC implementation are original.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOUSBHost/IOUSBHost.h>
#include "ring.h"
#include "power.h"
#include "rtl8157-init.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>

static volatile sig_atomic_t stopping = 0;
static void stop_signal(int) { stopping = 1; }
static uint32_t property(io_service_t s, CFStringRef key) {
    CFTypeRef v=IORegistryEntryCreateCFProperty(s,key,kCFAllocatorDefault,0);uint32_t r=0;
    if(v && CFGetTypeID(v)==CFNumberGetTypeID())CFNumberGetValue((CFNumberRef)v,kCFNumberSInt32Type,&r);
    if(v)CFRelease(v);return r;
}
static void require(bool ok, NSError *error, const char *operation) {
    if(!ok)throw std::runtime_error(std::string(operation)+": "+(error ? error.description.UTF8String : "failed"));
}

class Adapter {
    IOUSBHostDevice *__strong device_=nil;
    IOUSBHostInterface *__strong interface_=nil;
    IOUSBHostPipe *__strong input_=nil;
    NSMutableArray<NSMutableData *> *__strong buffers_=[NSMutableArray new];
    dispatch_queue_t queue_=dispatch_queue_create("org.ble-wide.usb-rx",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_USER_INTERACTIVE,0));
    dispatch_group_t pending_=dispatch_group_create();
    bool running_=false;
    usb_rx::TransferRing *ring_=nullptr;
    uint64_t transfers_=0, bytes_=0, errors_=0, completions_=0, timeouts_=0;
    std::atomic<bool> failed_{false};
    bool needs_init_=false;
    bool lease_=false;
    uint64_t heartbeat_=0;
    std::chrono::steady_clock::time_point last_heartbeat_;
    CapturePower *power_=nullptr;
    uint64_t power_generation_=0;
    bool resuming_=false;
    bool power_changed() const {return power_ && (power_->asleep() || power_->generation()!=power_generation_);}
public:
    Adapter(CapturePower *power=nullptr,uint64_t generation=0,bool resuming=false)
        :power_(power),power_generation_(generation),resuming_(resuming) {}
    ~Adapter() {
        stop();
        if(interface_)[interface_ destroy];
        if(device_)[device_ destroy]; // reset and re-register the stock driver
    }
    void open(uint32_t location,bool reconfigure=false) {
        io_iterator_t it=0;require(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&it)==0,nil,"enumerate USB");
        io_service_t found=0,s;unsigned count=0;
        while((s=IOIteratorNext(it))) {
            if(property(s,CFSTR("idVendor"))==0x0bda && property(s,CFSTR("idProduct"))==0x8157 &&
               (!location || property(s,CFSTR("locationID"))==location)) {
                if(!found){found=s;IOObjectRetain(found);}++count;
            }
            IOObjectRelease(s);
        }
        IOObjectRelease(it);
        if(count!=1){if(found)IOObjectRelease(found);throw std::runtime_error("expected exactly one matching RTL8157; use --location to disambiguate");}
        const auto revision=property(found,CFSTR("bcdDevice"));
        if(revision!=0x3000){IOObjectRelease(found);throw std::runtime_error("unsupported RTL8157 USB revision (requires 0x3000)");}
        printf("Capturing RTL8157 location=0x%08x revision=0x%04x uid=%u\n",property(found,CFSTR("locationID")),revision,geteuid());
        NSError *e=nil;
        device_=[[IOUSBHostDevice alloc] initWithIOService:found
            options:(IOUSBHostObjectInitOptionsDeviceCapture|IOUSBHostObjectInitOptionsDeviceSeize)
            queue:queue_ error:&e interestHandler:nil];
        IOObjectRelease(found);require(device_!=nil,e,"capture RTL8157");
        const auto *config=device_.configurationDescriptor;
        const bool change_config=!config || config->bConfigurationValue!=1;
        // An opened interface holds the device awake; configuring immediately
        // after capture returned kIOReturnNotReady on the tested Mac.
        if(config)open_interface();
        if(reconfigure) {
            require([device_ configureWithValue:0 matchInterfaces:NO error:&e],e,"test USB unconfiguration");
            puts("RTL8157 test: USB unconfigured under exclusive capture");
        }
        if(change_config || reconfigure) {
            require([device_ configureWithValue:1 matchInterfaces:NO error:&e],e,"select vendor configuration 1");
            if(interface_){[interface_ destroy];interface_=nil;}
            needs_init_=true;
            puts("RTL8157 vendor USB configuration selected under exclusive capture");
        }
        if(!interface_)open_interface();
        require([interface_ setIdleTimeout:0 error:&e],e,"disable interface idle suspension");
        input_=[interface_ copyPipeWithAddress:0x81 error:&e];require(input_!=nil,e,"open bulk input 0x81");
        rtl8157::Initializer<Adapter>(*this).validate();
    }
    void open_interface() {
        NSError *e=nil;io_service_t s=0;
        // Capture and interface registration can finish asynchronously.
        for(unsigned attempt=0;attempt<20 && !interface_;++attempt) {
            io_iterator_t children=0;
            if(IORegistryEntryGetChildIterator(device_.ioService,kIOServicePlane,&children)==0) {
                while((s=IOIteratorNext(children))) {
                    if(IOObjectConformsTo(s,"IOUSBHostInterface") && property(s,CFSTR("bInterfaceNumber"))==0) {
                        interface_=[[IOUSBHostInterface alloc] initWithIOService:s options:IOUSBHostObjectInitOptionsNone queue:queue_ error:&e interestHandler:nil];
                    }
                    IOObjectRelease(s);
                    if(interface_)break;
                }
                IOObjectRelease(children);
            }
            if(!interface_)usleep(50000);
        }
        require(interface_!=nil,e,"open vendor interface 0");
    }
    void pause(unsigned ms) {
        if(power_changed())throw std::runtime_error("USB initialization interrupted by sleep/reconnect");
        if(stopping)throw std::runtime_error("USB initialization cancelled");
        if(lease_) {
            if(usb_rx::acquire(ring_->header->stop_requested))throw std::runtime_error("USB capture stopped by viewer");
            const uint64_t current=usb_rx::acquire(ring_->header->heartbeat);
            const auto now=std::chrono::steady_clock::now();
            if(current!=heartbeat_){heartbeat_=current;last_heartbeat_=now;}
            if(now-last_heartbeat_>std::chrono::seconds(5))throw std::runtime_error("Viewer heartbeat stopped during initialization");
        }
        if(ms)std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    uint32_t read(uint16_t space,uint16_t address) {
        pause(0);
        NSError *e=nil;NSMutableData *data=[device_ ioDataWithCapacity:4 error:&e];require(data!=nil,e,"allocate control buffer");
        IOUSBDeviceRequest req={0xc0,5,uint16_t(address&~3),space,4};NSUInteger n=0;
        bool ok=[device_ sendDeviceRequest:req data:data bytesTransferred:&n completionTimeout:1 error:&e];
        require(ok,e,"read Realtek register");require(n==4,nil,"short Realtek register read");
        uint32_t v;memcpy(&v,data.bytes,4);return v;
    }
    void write(uint16_t space,uint16_t address,unsigned width,uint32_t value) {
        pause(0);
        const unsigned lane=address&3;
        if((width!=1 && width!=2 && width!=4) || lane+width>4)throw std::runtime_error("invalid register width");
        const uint16_t lanes=((1u<<width)-1)<<lane;
        IOUSBDeviceRequest req={0x40,5,uint16_t(address&~3),uint16_t(space|lanes|(lanes<<4)),4};
        NSError *e=nil;NSMutableData *data=[device_ ioDataWithCapacity:4 error:&e];require(data!=nil,e,"allocate control buffer");
        value<<=lane*8;memcpy(data.mutableBytes,&value,4);NSUInteger n=0;
        require([device_ sendDeviceRequest:req data:data bytesTransferred:&n completionTimeout:1 error:&e],e,"write Realtek register");
        require(n==4,nil,"short Realtek register write");
    }
    void block(uint16_t space,uint16_t address,const uint8_t *bytes,size_t length) {
        if(!length || (address&3) || (length&3) || length>0x10000u-address)
            throw std::runtime_error("invalid RTL8157 firmware block");
        // Realtek generic_ocp_write caps each vendor request at 512 bytes.
        while(length) {
            pause(0);
            const uint16_t size=uint16_t(std::min(length,size_t(512)));
            NSError *e=nil;NSMutableData *data=[device_ ioDataWithCapacity:size error:&e];
            require(data!=nil,e,"allocate firmware control buffer");memcpy(data.mutableBytes,bytes,size);
            IOUSBDeviceRequest req={0x40,5,address,uint16_t(space|0xff),size};NSUInteger n=0;
            require([device_ sendDeviceRequest:req data:data bytesTransferred:&n completionTimeout:1 error:&e],e,"upload RTL8157 firmware block");
            require(n==size,nil,"short RTL8157 firmware block write");
            length-=size;address+=size;bytes+=size;
        }
    }
    uint32_t field(uint16_t space,uint16_t address,unsigned width) {
        uint32_t v=read(space,address)>>((address&3)*8);
        return width==4 ? v : v & ((1u<<(width*8))-1);
    }
    void modify(uint16_t space,uint16_t address,unsigned width,uint32_t clear,uint32_t set) {
        const uint32_t old=field(space,address,width); // never write after a failed read
        write(space,address,width,(old&~clear)|set);
    }
    void diagnose() {
        for(uint16_t address : {0xc000,0xc004,0xc010,0xc014,0xc0a0,0xc0a4,0xc0a8,0xc0b4,0xe0cc,0xe610,0xe810,0xe84c,0xe858,0xe8dc,0xe908})
            printf("PLA[%04x]=%08x\n",address,read(0x100,address));
        for(uint16_t address : {0xd404,0xd40c,0xd42c,0xd430,0xd434,0xd4b0,0xd4ec})
            printf("USB[%04x]=%08x\n",address,read(0,address));
    }
    void warm_enable(unsigned aggregate_kib,unsigned coalesce_us) {
        // Shared final receive setup after warm takeover or independent PHY init.
        const uint16_t speed=field(0x100,0xe908,2);
        if(!(speed&2))throw std::runtime_error("PHY link lost before receive enable");
        printf("PHY status=%04x speed=%u Mb/s\n",speed,(speed&0x1000)?5000:(speed&0x400)?2500:0);
        modify(0x100,0xe85a,2,0,8); // gate RX while configuring aggregation
        // Driver termination may leave the NIC receiving into the out-of-band
        // wake engine instead of the host DMA engine (rtl8156_up).
        modify(0x100,0xe84f,1,0x80,0); // NOW_IS_OOB
        modify(0x100,0xc012,2,8,0);
        modify(0x100,0xe8de,2,0x4000,0); // MCU_BORW_EN
        modify(0x100,0xe0cc,2,0x4000,0); // PLA_MCU_SPDWN_EN
        write(0x100,0xc016,2,9216); // receive maximum frame size
        modify(0,0xd406,2,0x10,0); // enable receive aggregation
        modify(0,0xd4ee,2,0x1ff,0);
        write(0,0xd42c,2,80); // 640 ns early timeout, as in rtl8156_enable
        write(0,0xd42e,2,(aggregate_kib*1024-9216-16-16)/16); // RTL8157 uses 16-byte units
        write(0,0xd432,2,coalesce_us*1000/8); // 8 ns units, bounded to the 16-bit register
        modify(0x100,0xc0b4,2,1,0); // reset packet filter
        modify(0x100,0xc0b4,2,0,1);
        modify(0x100,0xc010,4,0x0f,0x0a); // physical MAC and broadcast only
        modify(0x100,0xe813,1,0,0x0c); // RX/TX enable
        write(0,0xd437,1,3); // publish RX DMA ownership change
        // Keep RX gated until the host has queued its DMA buffers.
    }
    void submit(NSMutableData *buffer) {
        NSError *e=nil;dispatch_group_enter(pending_);
        bool ok=[input_ enqueueIORequestWithData:buffer completionTimeout:1 error:&e
            completionHandler:^(IOReturn status, NSUInteger n) {
                ++completions_;
                if(status==kIOReturnTimeout)++timeouts_;
                if(completions_==1)printf("First USB completion: status=0x%08x bytes=%zu\n",status,(size_t)n);
                if(status==kIOReturnSuccess && n) {
                    ++transfers_;bytes_+=n;
                    if(ring_) {
                        usb_rx::add(ring_->header->transfers);usb_rx::add(ring_->header->usb_bytes,n);
                        if(n<=usb_rx::slot_bytes)ring_->push((const uint8_t*)buffer.bytes,n);
                        else {usb_rx::add(ring_->header->errors);failed_=true;}
                    }
                    if(transfers_==1) {
                        const uint8_t *p=(const uint8_t*)buffer.bytes;printf("First USB transfer: %zu bytes;",(size_t)n);
                        for(size_t k=0;k<32 && k<n;++k)printf(" %02x",p[k]);puts("");
                    }
                } else if(status!=kIOReturnSuccess && status!=kIOReturnTimeout && status!=kIOReturnAborted) {
                    ++errors_;failed_=true;if(ring_)usb_rx::add(ring_->header->errors);
                    fprintf(stderr,"USB input completion: 0x%08x\n",status);
                }
                if(running_ && !failed_)submit(buffer);
                dispatch_group_leave(pending_);
            }];
        if(!ok) {
            dispatch_group_leave(pending_);failed_=true;
            fprintf(stderr,"USB input submission: %s\n",e.description.UTF8String);
        }
    }
    void initialize(bool power_down,unsigned link_seconds) {
        rtl8157::Initializer<Adapter> init(*this);
        if(power_down)init.power_down_for_test();
        init.initialize(link_seconds);
    }
    void phy_diagnostics() {rtl8157::Initializer<Adapter>(*this).diagnostics();}
    void receive(unsigned seconds,usb_rx::TransferRing *ring,bool passive,unsigned aggregate_kib,unsigned coalesce_us,unsigned requests,bool lease,
                 bool force_init,bool power_down,unsigned link_seconds) {
        ring_=ring;
        lease_=lease;last_heartbeat_=std::chrono::steady_clock::now();
        heartbeat_=ring_?usb_rx::acquire(ring_->header->heartbeat):0;
        if(!passive && (force_init || power_down || needs_init_ || !(field(0x100,0xe908,2)&2))) {
            if(ring_) {usb_rx::release(ring_->header->pid,getpid());usb_rx::release(ring_->header->state,3);}
            initialize(power_down,link_seconds);
        }
        for(unsigned k=0;k<requests;++k) {
            NSError *e=nil;NSMutableData *b=[interface_ ioDataWithCapacity:aggregate_kib*1024 error:&e];
            require(b!=nil,e,"allocate USB DMA buffer");[buffers_ addObject:b];
            if(k==0)printf("USB DMA buffer length=%zu\n",(size_t)b.length);
        }
        if(!passive)warm_enable(aggregate_kib,coalesce_us);
        if(ring_) {usb_rx::release(ring_->header->pid,getpid());usb_rx::release(ring_->header->state,1);}
        rusage before{},after{};getrusage(RUSAGE_SELF,&before);
        dispatch_sync(queue_,^{running_=true;for(NSMutableData *b in buffers_)submit(b);});
        if(!passive)modify(0x100,0xe85a,2,8,0); // ungate RX with all requests ready
        const auto start=std::chrono::steady_clock::now();
        auto last_heartbeat=start;
        auto last_poll=start;
        uint64_t heartbeat=ring_?usb_rx::acquire(ring_->header->heartbeat):0;
        while(!stopping && !failed_ && std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)) {
            if(power_)power_->poll();
            if(power_changed())break;
            if(lease) {
                if(usb_rx::acquire(ring_->header->stop_requested))break;
                const auto now=std::chrono::steady_clock::now();
                // The capture thread can wake before the power-notification
                // queue. Allow one normal lease interval for both to resume.
                if(now-last_poll>std::chrono::seconds(5))last_heartbeat=now;
                last_poll=now;
                const uint64_t current=usb_rx::acquire(ring_->header->heartbeat);
                if(current!=heartbeat){heartbeat=current;last_heartbeat=now;}
                if(now-last_heartbeat>std::chrono::seconds(5)) {
                    fprintf(stderr,"Viewer heartbeat stopped; releasing USB adapter\n");failed_=true;break;
                }
            }
            usleep(10000);
        }
        stop();
        // The session owner reopens the adapter after wake. Do not perform
        // register reads against a sleeping/disconnected device or expire its lease.
        if(power_changed())return;
        getrusage(RUSAGE_SELF,&after);
        printf("USB completions=%llu timeouts=%llu\n",(unsigned long long)completions_,(unsigned long long)timeouts_);
        diagnose();
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        const auto cpu_seconds=[](const timeval &v){return double(v.tv_sec)+v.tv_usec/1e6;};
        const double cpu=cpu_seconds(after.ru_utime)+cpu_seconds(after.ru_stime)-cpu_seconds(before.ru_utime)-cpu_seconds(before.ru_stime);
        printf("{\"seconds\":%.6f,\"usb_transfers\":%llu,\"usb_bytes\":%llu,\"mbps\":%.3f,\"errors\":%llu,\"ring_drops\":%llu,\"timeouts\":%llu,\"cpu_seconds\":%.6f,\"aggregate_kib\":%u,\"coalesce_us\":%u,\"requests\":%u}\n",
            elapsed,(unsigned long long)transfers_,(unsigned long long)bytes_,bytes_*8/elapsed/1e6,
            (unsigned long long)errors_,(unsigned long long)(ring_?usb_rx::acquire(ring_->header->dropped):0),
            (unsigned long long)timeouts_,cpu,aggregate_kib,coalesce_us,requests);
        if(failed_)throw std::runtime_error("USB receive failed");
        if(!transfers_)throw std::runtime_error("no USB frames received during capture");
    }
    void stop() {
        if(!input_)return;
        dispatch_sync(queue_,^{running_=false;});
        NSError *e=nil;[input_ abortWithOption:IOUSBHostAbortOptionSynchronous error:&e];
        if(dispatch_group_wait(pending_,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC))) {
            // Never destroy buffers while a callback could still reference them.
            fprintf(stderr,"USB callbacks did not drain; exiting for kernel cleanup\n");_Exit(1);
        }
        if(ring_)usb_rx::release(ring_->header->state,
            power_changed()?(power_->asleep()?4:5):(resuming_?5:2));
    }
};

int main(int argc,char **argv) {
    try { @autoreleasepool { @try {
        setbuf(stdout,nullptr);signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);
        bool receive=false,passive=false,lease=false,force_init=false,power_down=false,reconfigure=false;
        unsigned seconds=10,aggregate_kib=256,coalesce_us=85,requests=32,link_seconds=30;uint32_t location=0;uid_t uid=0;std::string ring_path;
        if(const char *u=getenv("BLE_USB_CLIENT_UID"))uid=uid_t(std::stoul(u));
        else if(const char *u=getenv("SUDO_UID"))uid=uid_t(std::stoul(u));
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];auto value=[&]() {if(i+1>=argc)throw std::runtime_error("missing value for "+arg);return std::string(argv[++i]);};
            if(arg=="--probe" || arg=="--capture"){}
            else if(arg=="--receive")receive=true;
            else if(arg=="--passive")passive=true;
            else if(arg=="--lease")lease=true;
            else if(arg=="--initialize")force_init=true;
            else if(arg=="--test-phy-power-down")power_down=true;
            else if(arg=="--test-reconfigure")reconfigure=true;
            else if(arg=="--link-seconds")link_seconds=std::stoul(value());
            else if(arg=="--seconds")seconds=std::stoul(value());
            else if(arg=="--requests")requests=std::stoul(value());
            else if(arg=="--coalesce-us")coalesce_us=std::stoul(value());
            else if(arg=="--aggregate-kib")aggregate_kib=std::stoul(value());
            else if(arg=="--location")location=std::stoul(value(),nullptr,0);
            else if(arg=="--uid")uid=std::stoul(value());
            else if(arg=="--ring")ring_path=value();
            else if(arg=="--help") {puts("rtl8157-helper [--probe | --receive] [--seconds N] [--ring PATH --uid N] [--location HEX] [--aggregate-kib 32|64|128|256] [--coalesce-us 1..524] [--requests 8|16|32] [--passive] [--lease] [--initialize] [--test-phy-power-down] [--test-reconfigure] [--link-seconds 1..60]");return 0;}
            else throw std::runtime_error("unknown argument: "+arg);
        }
        if(!seconds || seconds>86400)throw std::runtime_error("seconds must be 1..86400");
        if(aggregate_kib!=32 && aggregate_kib!=64 && aggregate_kib!=128 && aggregate_kib!=256)throw std::runtime_error("aggregate-kib must be 32, 64, 128 or 256");
        if(requests!=8 && requests!=16 && requests!=32)throw std::runtime_error("requests must be 8, 16 or 32");
        if(!coalesce_us || coalesce_us>524)throw std::runtime_error("coalesce-us must be 1..524");
        if(!link_seconds || link_seconds>60)throw std::runtime_error("link-seconds must be 1..60");
        if(passive && (force_init || power_down || reconfigure))throw std::runtime_error("--passive cannot initialize the adapter");
        if(lease && (!receive || ring_path.empty()))throw std::runtime_error("--lease requires --receive and --ring");
        if(geteuid())throw std::runtime_error("USB capture requires root; SIP and startup security stay enabled");
        // Ring must outlive Adapter, whose destructor drains callbacks before releasing USB.
        std::unique_ptr<usb_rx::TransferRing> ring;
        if(!ring_path.empty()) {
            if(!uid)throw std::runtime_error("--uid must identify the unprivileged receiver user");
            ring=std::make_unique<usb_rx::TransferRing>(ring_path,true,uid);
        }
        std::unique_ptr<CapturePower> power;
        if(lease)power=std::make_unique<CapturePower>(ring->header);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
        auto retry_until=deadline;
        bool recovering=false;
        uint64_t recovery_generation=0;
        auto cancelled=[&] {return stopping || (lease && usb_rx::acquire(ring->header->stop_requested));};
        while(!cancelled() && std::chrono::steady_clock::now()<deadline) {
            if(power)power->poll();
            const uint64_t generation=power?power->generation():0;
            if(power && (power->asleep() || recovering || generation!=recovery_generation)) {
                recovering=true;
                usb_rx::release(ring->header->state,power->asleep()?4:5);
                while(power->asleep() && !cancelled() && std::chrono::steady_clock::now()<deadline)usleep(100000);
                if(cancelled() || std::chrono::steady_clock::now()>=deadline)break;
                if(generation!=recovery_generation) {
                    recovery_generation=generation;
                    retry_until=std::chrono::steady_clock::now()+std::chrono::seconds(45);
                }
                // Give the unprivileged viewer a chance to resume before checking
                // its normal five-second lease. A dead viewer still releases USB.
                const uint64_t heartbeat=usb_rx::acquire(ring->header->heartbeat);
                const auto heartbeat_until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
                while(heartbeat==usb_rx::acquire(ring->header->heartbeat) && !cancelled() &&
                      !power->asleep() && std::chrono::steady_clock::now()<heartbeat_until)usleep(100000);
                if(cancelled())break;
                if(power->asleep())continue;
                if(heartbeat==usb_rx::acquire(ring->header->heartbeat))
                    throw std::runtime_error("Viewer heartbeat did not resume after wake");
                usb_rx::release(ring->header->state,5);
                fprintf(stderr,"Reopening RTL8157 in the existing authorized session\n");
            }
            try {
                Adapter adapter(power.get(),generation,recovering);
                adapter.open(location,reconfigure && !recovering);adapter.diagnose();adapter.phy_diagnostics();
                const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(deadline-std::chrono::steady_clock::now()).count();
                if(remaining<=0)break;
                if(receive)adapter.receive(unsigned(remaining),ring.get(),passive,aggregate_kib,coalesce_us,requests,lease,
                                           force_init,power_down && !recovering,link_seconds);
                else if(force_init || power_down || reconfigure)adapter.initialize(power_down,link_seconds);
            } catch(const std::exception &e) {
                if(power && (power->asleep() || power->generation()!=generation)) {
                    recovering=true;continue;
                }
                if(recovering && !cancelled() && std::chrono::steady_clock::now()<retry_until) {
                    fprintf(stderr,"USB wake retry: %s\n",e.what());usleep(500000);continue;
                }
                throw;
            }
            if(power && (power->asleep() || power->generation()!=generation)) {
                recovering=true;continue;
            }
            break;
        }
        if(ring)usb_rx::release(ring->header->state,2);
        puts("Releasing USB adapter to its normal driver.");return 0;
    } @catch(NSException *e) {fprintf(stderr,"USB framework exception: %s\n",e.description.UTF8String);return 1;}
    } } catch(const std::exception &e) {fprintf(stderr,"rtl8157-helper: %s\n",e.what());return 1;}
}
