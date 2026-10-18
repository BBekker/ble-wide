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
public:
    ~Adapter() {
        stop();
        if(interface_)[interface_ destroy];
        if(device_)[device_ destroy]; // reset and re-register the stock driver
    }
    void open(uint32_t location) {
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
        printf("Capturing RTL8157 location=0x%08x revision=0x%04x uid=%u\n",property(found,CFSTR("locationID")),property(found,CFSTR("bcdDevice")),geteuid());
        NSError *e=nil;
        device_=[[IOUSBHostDevice alloc] initWithIOService:found
            options:(IOUSBHostObjectInitOptionsDeviceCapture|IOUSBHostObjectInitOptionsDeviceSeize)
            queue:queue_ error:&e interestHandler:nil];
        IOObjectRelease(found);require(device_!=nil,e,"capture RTL8157");
        const auto *config=device_.configurationDescriptor;
        if(!config || config->bConfigurationValue!=1)throw std::runtime_error("requires initialized vendor configuration 1; first connect with the Realtek driver");
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
        require([interface_ setIdleTimeout:0 error:&e],e,"disable interface idle suspension");
        input_=[interface_ copyPipeWithAddress:0x81 error:&e];require(input_!=nil,e,"open bulk input 0x81");
    }
    uint32_t read(uint16_t space,uint16_t address) {
        NSError *e=nil;NSMutableData *data=[device_ ioDataWithCapacity:4 error:&e];require(data!=nil,e,"allocate control buffer");
        IOUSBDeviceRequest req={0xc0,5,uint16_t(address&~3),space,4};NSUInteger n=0;
        bool ok=[device_ sendDeviceRequest:req data:data bytesTransferred:&n completionTimeout:1 error:&e];
        require(ok,e,"read Realtek register");require(n==4,nil,"short Realtek register read");
        uint32_t v;memcpy(&v,data.bytes,4);return v;
    }
    void write(uint16_t space,uint16_t address,unsigned width,uint32_t value) {
        const unsigned lane=address&3;
        if((width!=1 && width!=2 && width!=4) || lane+width>4)throw std::runtime_error("invalid register width");
        const uint16_t lanes=((1u<<width)-1)<<lane;
        IOUSBDeviceRequest req={0x40,5,uint16_t(address&~3),uint16_t(space|lanes|(lanes<<4)),4};
        NSError *e=nil;NSMutableData *data=[device_ ioDataWithCapacity:4 error:&e];require(data!=nil,e,"allocate control buffer");
        value<<=lane*8;memcpy(data.mutableBytes,&value,4);NSUInteger n=0;
        require([device_ sendDeviceRequest:req data:data bytesTransferred:&n completionTimeout:1 error:&e],e,"write Realtek register");
        require(n==4,nil,"short Realtek register write");
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
    void warm_enable(unsigned aggregate_kib) {
        // This is deliberately not a cold PHY/firmware loader. Keep the existing
        // calibration/link; refuse to guess when the stock driver left it down.
        const uint16_t speed=field(0x100,0xe908,2);
        if(!(speed&2))throw std::runtime_error("PHY link is down after capture; cold initialization is not implemented");
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
        write(0,0xd432,2,85000/8); // 85 us aggregation timer
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
    void receive(unsigned seconds,usb_rx::TransferRing *ring,bool passive,unsigned aggregate_kib,bool lease) {
        ring_=ring;
        for(unsigned k=0;k<32;++k) {
            NSError *e=nil;NSMutableData *b=[interface_ ioDataWithCapacity:usb_rx::slot_bytes error:&e];
            require(b!=nil,e,"allocate USB DMA buffer");[buffers_ addObject:b];
            if(k==0)printf("USB DMA buffer length=%zu\n",(size_t)b.length);
        }
        if(!passive)warm_enable(aggregate_kib);
        if(ring_) {usb_rx::release(ring_->header->pid,getpid());usb_rx::release(ring_->header->state,1);}
        rusage before{},after{};getrusage(RUSAGE_SELF,&before);
        dispatch_sync(queue_,^{running_=true;for(NSMutableData *b in buffers_)submit(b);});
        if(!passive)modify(0x100,0xe85a,2,8,0); // ungate RX with all requests ready
        const auto start=std::chrono::steady_clock::now();
        auto last_heartbeat=start;
        uint64_t heartbeat=ring_?usb_rx::acquire(ring_->header->heartbeat):0;
        while(!stopping && !failed_ && std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)) {
            if(lease) {
                if(usb_rx::acquire(ring_->header->stop_requested))break;
                const auto now=std::chrono::steady_clock::now();
                const uint64_t current=usb_rx::acquire(ring_->header->heartbeat);
                if(current!=heartbeat){heartbeat=current;last_heartbeat=now;}
                if(now-last_heartbeat>std::chrono::seconds(5)) {
                    fprintf(stderr,"Viewer heartbeat stopped; releasing USB adapter\n");failed_=true;break;
                }
            }
            usleep(10000);
        }
        stop();
        getrusage(RUSAGE_SELF,&after);
        printf("USB completions=%llu timeouts=%llu\n",(unsigned long long)completions_,(unsigned long long)timeouts_);
        diagnose();
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        const auto cpu_seconds=[](const timeval &v){return double(v.tv_sec)+v.tv_usec/1e6;};
        const double cpu=cpu_seconds(after.ru_utime)+cpu_seconds(after.ru_stime)-cpu_seconds(before.ru_utime)-cpu_seconds(before.ru_stime);
        printf("{\"seconds\":%.6f,\"usb_transfers\":%llu,\"usb_bytes\":%llu,\"mbps\":%.3f,\"errors\":%llu,\"ring_drops\":%llu,\"timeouts\":%llu,\"cpu_seconds\":%.6f,\"aggregate_kib\":%u}\n",
            elapsed,(unsigned long long)transfers_,(unsigned long long)bytes_,bytes_*8/elapsed/1e6,
            (unsigned long long)errors_,(unsigned long long)(ring_?usb_rx::acquire(ring_->header->dropped):0),
            (unsigned long long)timeouts_,cpu,aggregate_kib);
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
        if(ring_)usb_rx::release(ring_->header->state,2);
    }
};

int main(int argc,char **argv) {
    try { @autoreleasepool { @try {
        setbuf(stdout,nullptr);signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);
        bool receive=false,passive=false,lease=false;unsigned seconds=10,aggregate_kib=64;uint32_t location=0;uid_t uid=0;std::string ring_path;
        if(const char *u=getenv("BLE_USB_CLIENT_UID"))uid=uid_t(std::stoul(u));
        else if(const char *u=getenv("SUDO_UID"))uid=uid_t(std::stoul(u));
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];auto value=[&]() {if(i+1>=argc)throw std::runtime_error("missing value for "+arg);return std::string(argv[++i]);};
            if(arg=="--probe" || arg=="--capture"){}
            else if(arg=="--receive")receive=true;
            else if(arg=="--passive")passive=true;
            else if(arg=="--lease")lease=true;
            else if(arg=="--seconds")seconds=std::stoul(value());
            else if(arg=="--aggregate-kib")aggregate_kib=std::stoul(value());
            else if(arg=="--location")location=std::stoul(value(),nullptr,0);
            else if(arg=="--uid")uid=std::stoul(value());
            else if(arg=="--ring")ring_path=value();
            else if(arg=="--help") {puts("rtl8157-helper [--probe | --receive] [--seconds N] [--ring PATH --uid N] [--location HEX] [--aggregate-kib 32|64] [--passive] [--lease]");return 0;}
            else throw std::runtime_error("unknown argument: "+arg);
        }
        if(!seconds || seconds>86400)throw std::runtime_error("seconds must be 1..86400");
        if(aggregate_kib!=32 && aggregate_kib!=64)throw std::runtime_error("aggregate-kib must be 32 or 64");
        if(lease && (!receive || ring_path.empty()))throw std::runtime_error("--lease requires --receive and --ring");
        if(geteuid())throw std::runtime_error("USB capture requires root; SIP and startup security stay enabled");
        // Ring must outlive Adapter, whose destructor drains callbacks before releasing USB.
        std::unique_ptr<usb_rx::TransferRing> ring;
        if(!ring_path.empty()) {
            if(!uid)throw std::runtime_error("--uid must identify the unprivileged receiver user");
            ring=std::make_unique<usb_rx::TransferRing>(ring_path,true,uid);
        }
        Adapter adapter;adapter.open(location);adapter.diagnose();
        if(receive)adapter.receive(seconds,ring.get(),passive,aggregate_kib,lease);
        puts("Releasing USB adapter to its normal driver.");return 0;
    } @catch(NSException *e) {fprintf(stderr,"USB framework exception: %s\n",e.description.UTF8String);return 1;}
    } } catch(const std::exception &e) {fprintf(stderr,"rtl8157-helper: %s\n",e.what());return 1;}
}
