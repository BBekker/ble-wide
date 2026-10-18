# SPDX-License-Identifier: MIT
"""Lossless packet delivery to the UI, SSE resume, and real channel power."""
import importlib.util
import http.client
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
spec=importlib.util.spec_from_file_location('wide_viewer_server',ROOT/'server.py')
viewer=importlib.util.module_from_spec(spec);spec.loader.exec_module(viewer)


def packet(n):
    return dict(type='packet',sample=n*2000,capture_s=n*.001,epoch=1,
                channel=n%37,mhz=2404,access_address='12345678',crc_init='abcdef',
                crc='123456',context='repeated_crc_seed',pdu='0100')


class ViewerStreamTest(unittest.TestCase):
    def test_crc_failures_are_retained_without_trusting_their_contents(self):
        import copy
        state=viewer.State()
        state.ingest(packet(1))
        connection=state.tracker.current['12345678']
        before=copy.deepcopy(connection)
        failed=packet(2)
        failed.update(crc_valid=False,expected_crc='654321',pdu='03020200',
                      signal_dbfs=-45,noise_dbfs=-60)
        state.ingest(failed)
        delivered=state.snapshot(0)['packets'][-1]
        self.assertEqual(delivered['kind'],'CRC failed')
        self.assertFalse(delivered['crc_valid'])
        self.assertEqual(delivered['expected_crc'],'654321')
        self.assertEqual(delivered['connection_id'],connection['id'])
        self.assertEqual(connection,before)  # no terminate/map/timing/count updates
        unknown=packet(3);unknown.update(crc_valid=False,access_address='87654321')
        state.ingest(unknown)
        self.assertNotIn('connection_id',state.snapshot(0)['packets'][-1])
        self.assertEqual(len(state.tracker.connections),1)
        from test_ble_connections import request
        bad_setup=packet(4)
        bad_setup.update(crc_valid=False,access_address='8e89bed6',channel=37,
                         pdu=request().hex())
        state.ingest(bad_setup)
        self.assertEqual(len(state.tracker.connections),1)
        self.assertEqual(state.tracker.advertisers,{})

    def test_packet_noise_measurement_survives_viewer_delivery(self):
        state=viewer.State()
        p=packet(1);p.update(signal_dbfs=-35.2,noise_dbfs=-43.1)
        state.ingest(p)
        delivered=state.snapshot(0)['packets'][0]
        self.assertEqual(delivered['noise_dbfs'],-43.1)
        p=packet(2);p['noise_dbfs']=None;state.ingest(p)
        self.assertIsNone(state.snapshot(0)['packets'][-1]['noise_dbfs'])

    def test_every_packet_is_returned_beyond_old_200_row_limit(self):
        state=viewer.State()
        for n in range(2500):state.ingest(packet(n))
        data=state.snapshot(0)
        self.assertEqual([p['id'] for p in data['packets']],list(range(1,2501)))
        self.assertEqual([p['id'] for p in state.snapshot(2497)['packets']],[2498,2499,2500])
        self.assertFalse(data['history_expired'])

    def test_retention_expiry_is_explicit_and_spectrum_independent(self):
        state=viewer.State()
        from collections import deque
        state.packets=deque(maxlen=3)
        for n in range(5):state.ingest(packet(n))
        self.assertTrue(state.snapshot(1)['history_expired'])
        for n in range(4):state.ingest(dict(type='spectrum',capture_s=n*.02,end_s=(n+1)*.02,db=[-70]*40))
        data=state.snapshot(4,2)
        self.assertEqual([p['id'] for p in data['packets']],[5])
        self.assertEqual([s['id'] for s in data['spectrum']],[3,4])

    def test_sse_batch_and_reconnect_cursor(self):
        state=viewer.State()
        server=ThreadingHTTPServer(('127.0.0.1',0),viewer.make_handler(state))
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        def read(response):
            identifier=response.readline().decode().strip()[4:]
            data=json.loads(response.readline().decode()[6:]);response.readline()
            return identifier,data
        try:
            for n in range(350):state.ingest(packet(n))
            conn=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3)
            conn.request('GET','/events');resp=conn.getresponse()
            self.assertEqual(resp.getheader('Content-Type'),'text/event-stream')
            last,data=read(resp);self.assertEqual(len(data['packets']),350)
            for n in range(350,400):state.ingest(packet(n))
            _,data=read(resp)
            self.assertEqual([p['id'] for p in data['packets']],list(range(351,401)))
            resp.close();conn.close()
            conn=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3)
            conn.request('GET','/events',headers={'Last-Event-ID':last});resp=conn.getresponse()
            _,data=read(resp);self.assertEqual(len(data['packets']),50)
            resp.close();conn.close()
            conn=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3)
            conn.request('GET','/events',headers={'Last-Event-ID':'old-session:99999:99999'});resp=conn.getresponse()
            _,data=read(resp);self.assertEqual(len(data['packets']),400)
            resp.close();conn.close()
        finally:
            server.shutdown();server.server_close();thread.join()

    def test_fft_power_and_gap_reset(self):
        code=r'''
#include "waterfall.h"
#include <cassert>
int main(){
 Waterfall w; float bins[80]{}; bins[0]=8192; bins[2]=4096;
 int calls=0;
 auto output=[&](double start,double end,const auto& db){
   calls++; assert(std::abs(db[0])<1e-5); assert(std::abs(db[1]+6.0206)<.001);
   assert(db[2]==-120); assert(std::abs(end-start-.02)<1e-9);
 };
 for(unsigned i=0;i<40000;i++) w.run(bins,i*.5e-6,output);
 assert(calls==1);
 for(unsigned i=0;i<20000;i++) w.run(bins,.02+i*.5e-6,output);
 w.reset();
 for(unsigned i=0;i<20000;i++) w.run(bins,1+i*.5e-6,output);
 assert(calls==1);
 for(unsigned i=20000;i<40000;i++) w.run(bins,1+i*.5e-6,output);
 assert(calls==2);
}'''
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'test.cpp';src.write_text(code)
            subprocess.run(['c++','-O2','-std=c++17','-I',str(ROOT),str(src),'-o',tmp+'/test'],check=True)
            subprocess.run([tmp+'/test'],check=True)

if __name__=='__main__':unittest.main()
