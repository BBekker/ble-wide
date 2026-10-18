// SPDX-License-Identifier: MIT
const assert=require('node:assert/strict');
const {decode,encryptionState}=require('../../web/packet-model.js');
const connection={observed:true,start:0,encrypted:false,markers:[],warning:''};
const frame=(payload,llid=2)=>({id:10,capture_s:1,access_address:'12345678',pdu:llid.toString(16).padStart(2,'0')+(payload.length/2).toString(16).padStart(2,'0')+payload});
const l2=(body,cid=4)=>frame((body.length/2).toString(16).padStart(2,'0')+'00'+cid.toString(16).padStart(2,'0')+'00'+body);
const fields=r=>Object.fromEntries(r.fields);
let r=decode(l2('12b20092'),connection);
assert.equal(r.name,'Write request');assert.equal(fields(r).Handle,'0x00b2');assert.equal(fields(r).Value,'92');
r=decode(l2('1b250048656c6c6f'),connection);
assert.equal(r.name,'Notification');assert.equal(fields(r)['Value · text'],'Hello');
r=decode(l2('1300'),connection);assert.equal(r.status,'malformed');
r=decode(l2('13'),connection);assert.equal(r.name,'Write response');
r=decode(l2('02f700'),connection);assert.equal(fields(r)['Receive MTU'],'247 bytes');
r=decode(l2('010a20000a'),connection);assert.equal(fields(r).Error,'Attribute not found (0x0a)');
r=decode(l2('100100ffff0028'),connection);assert.equal(fields(r)['Attribute type UUID'],'0x2800');
r=decode(l2('05010100002802000328'),connection);assert.equal(fields(r)['Handle 0x0001'],'UUID 0x2800');
r=decode(l2('16010002004142'),connection);assert.equal(fields(r)['Value offset'],'2');assert.equal(fields(r)['Value · text'],'AB');
r=decode(l2('230100010041020002004243'),connection);assert.equal(r.name,'Multiple notifications');assert.equal(fields(r)['0x0002 · text'],'BC');
r=decode(l2('120108000c0018000000c800',5),connection);assert.equal(fields(r)['Interval range'],'15–30 ms');
r=decode(frame('01ffffffff1f3412',3),connection);assert.equal(r.name,'LL_CHANNEL_MAP_IND');assert.equal(fields(r).Instant,'4660');assert.equal(fields(r)['Enabled channels'].split(', ').length,37);
r=decode(frame('2400f50800',3),connection);assert.equal(fields(r)['Actual power change'],'-11 dB');assert.equal(fields(r)['Sender TX power'],'8 dBm');
r=decode(frame('140001480801014808',3),connection);assert.equal(fields(r)['Max RX octets'],'256');
r=decode(frame('2955555555555555555501',3),connection);assert.equal(fields(r)['Good channels'].split(', ').length,37);
r=decode(frame('050004001201'),connection);assert.equal(r.status,'fragment');
r=decode(frame('aabb',1),connection);assert.equal(r.status,'fragment');
r=decode(frame('',1),{encrypted:true});assert.equal(r.name,'Empty data PDU');
// Security context must never turn ciphertext into plausible protocol messages.
r=decode(l2('13'),{...connection,encrypted:true});assert.equal(r.status,'encrypted');
r=decode(l2('13'),{...connection,encrypted:true},[],true);assert.equal(r.status,'encrypted');
r=decode(l2('13'),{observed:false});assert.equal(r.status,'unknown');
r=decode(l2('13'),{observed:false},[],true);assert.equal(r.name,'Write response');assert.match(r.warnings[0],/Assumed plaintext/);
r=decode(l2('13'),connection,[{start:.5,end:.6}]);assert.equal(r.status,'unknown');
const secure={...connection,encrypted:true,markers:[{kind:'encryption',time:2,id:20}]};
assert.equal(encryptionState(l2('13'),secure,[]),'plaintext');
assert.equal(encryptionState({...l2('13'),capture_s:3,id:30},secure,[]),'encrypted');
// Missing, malformed, reserved and truncated fields fail closed without exceptions.
for(const pdu of ['','0','zz','00ff','020100','2200','02090400040012b20092'])assert.equal(decode({...frame(''),pdu},connection).status,'malformed',pdu);
for(const body of ['05','0501','0900','09030200','1100','21030001','230100050041','d20100'])assert.equal(decode(l2(body),connection).status,'malformed',body);
assert.equal(decode(l2('ff0102'),connection).status,'unsupported');
assert.equal(decode(l2('010203',64),connection).status,'unsupported');
// Exercise all opcode/length combinations, including random bytes, for bounds safety.
for(let op=0;op<256;op++)for(let len=1;len<28;len++){
  const body=Array.from({length:len},(_,i)=>(i?((op*17+i*31)&255):op).toString(16).padStart(2,'0')).join('');
  for(const packet of [l2(body),frame(body,3)])assert.doesNotThrow(()=>decode(packet,connection));
}
console.log('Browser LL, L2CAP, ATT, security and malformed-packet tests passed');
