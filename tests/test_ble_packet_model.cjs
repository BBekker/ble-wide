// SPDX-License-Identifier: MIT
const assert=require('node:assert/strict');
const {describe,group}=require('../web/packet-model.js');
const packet=(id,pdu='0100',epoch=1)=>({id,pdu,epoch,capture_s:id*.01,channel:id%37,access_address:'12345678',signal_dbfs:-40+id});
assert.equal(describe(packet(1)).empty,true);
assert.equal(describe(packet(2,'1d00')).empty,true); // SN/NESN/MD do not change emptiness.
assert.equal(describe(packet(3,'010411223344')).empty,false); // A MIC-sized payload is still data.
assert.equal(describe(packet(4,'0300')).empty,false); // Zero-length control is not an empty data PDU.
assert.equal(describe({...packet(5),access_address:'8e89bed6'}).empty,false);
const packets=[packet(1),packet(2,'1d00'),packet(3,'0202aabb'),packet(4),packet(5)];
let groups=group(packets);
assert.deepEqual(groups.map(g=>g.packets.length),[2,1,2]);
assert.deepEqual(groups.flatMap(g=>g.packets),packets);
assert.equal(describe(packets[2]).payload,'aabb');
assert.equal(describe(packets[1]).sn,1);
assert.equal(describe(packets[1]).nesn,1);
assert.equal(describe(packets[1]).md,1);
assert.deepEqual(group(packets,[{start:.014,end:.016}]).map(g=>g.packets.length),[1,1,1,2]);
assert.equal(group([packet(1),packet(2,'0100',2)]).length,2);
const many=Array.from({length:50000},(_,i)=>packet(i+1));
groups=group(many);
assert.equal(groups.length,1);assert.equal(groups[0].packets.length,50000);
assert.equal(groups[0].packets[49999].id,50000);
console.log('Connection packet grouping tests passed');
const failed={...packet(1),crc_valid:false};
assert.equal(describe(failed).empty,false);
assert.equal(describe(failed).kind,'CRC failed');
assert.deepEqual(group([packet(1),{...packet(2),crc_valid:false},packet(3)]).map(g=>g.packets.length),[1,1,1]);
const decoded=require('../web/packet-model.js').decode(failed,null,[],true);
assert.equal(decoded.status,'crc_failed');
assert.equal(decoded.fields.length,0);
assert.match(decoded.warnings[0],/CRC mismatch/);
