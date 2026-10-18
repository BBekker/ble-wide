// SPDX-License-Identifier: MIT
// Sleep notifications never veto sleep. Keep authorization alive across a wake.
#pragma once
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include "ring.h"

class CapturePower {
    usb_rx::Header *header_;
    io_connect_t root_=IO_OBJECT_NULL;
    io_object_t notifier_=IO_OBJECT_NULL;
    IONotificationPortRef port_=nullptr;
    dispatch_queue_t queue_=dispatch_queue_create("org.ble-wide.power",DISPATCH_QUEUE_SERIAL);
    std::atomic<bool> asleep_{false};
    std::atomic<uint64_t> generation_{0};
    uint64_t reconnect_;
    static void notification(void *context,io_service_t,natural_t type,void *argument) {
        auto &self=*static_cast<CapturePower *>(context);
        if(type==kIOMessageSystemWillSleep) {
            self.asleep_=true;
            ++self.generation_;
            usb_rx::release(self.header_->state,4);
            fprintf(stderr,"System sleeping; suspending direct USB capture\n");
        } else if(type==kIOMessageSystemHasPoweredOn) {
            usb_rx::release(self.header_->state,5);
            self.asleep_=false;
            fprintf(stderr,"System woke; reopening direct USB capture\n");
        }
        if(type==kIOMessageCanSystemSleep || type==kIOMessageSystemWillSleep)
            IOAllowPowerChange(self.root_,reinterpret_cast<intptr_t>(argument));
    }
public:
    explicit CapturePower(usb_rx::Header *header):header_(header),reconnect_(usb_rx::acquire(header->reconnect_requested)) {
        root_=IORegisterForSystemPower(this,&port_,notification,&notifier_);
        if(!root_) {
            if(port_)IONotificationPortDestroy(port_);
            throw std::runtime_error("cannot register USB sleep/wake notifications");
        }
        IONotificationPortSetDispatchQueue(port_,queue_);
    }
    ~CapturePower() {
        IODeregisterForSystemPower(&notifier_);
        IONotificationPortDestroy(port_);
        dispatch_sync(queue_,^{});
        IOServiceClose(root_);
    }
    bool asleep() const {return asleep_.load();}
    uint64_t generation() const {return generation_.load();}
    void poll() {
        const auto request=usb_rx::acquire(header_->reconnect_requested);
        if(request!=reconnect_) {
            reconnect_=request;++generation_;
            usb_rx::release(header_->state,5);
            fprintf(stderr,"Viewer requested direct USB reconnect\n");
        }
    }
};
