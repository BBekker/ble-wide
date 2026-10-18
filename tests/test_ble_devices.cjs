const assert=require('node:assert/strict');
const {advertisement,DeviceRegistry}=require('../web/packet-model.js');
function packet(name='',type=0,random=false,complete=true,address='010203040506'){
  const text=Buffer.from(name),ad=name?Buffer.concat([Buffer.from([text.length+1,complete?9:8]),text]):Buffer.alloc(0);
  const body=Buffer.concat([Buffer.from(address,'hex'),ad]);
  return {access_address:'8e89bed6',pdu:Buffer.concat([Buffer.from([type|(random?64:0),body.length]),body]).toString('hex'),capture_s:1,channel:37,signal_dbfs:-42};
}
const registry=new DeviceRegistry();
let p=packet('Hear',0,true,false);let d=registry.ingest(p);
assert.equal(d.address,'06:05:04:03:02:01');assert.equal(d.name,'Hear');assert.equal(d.complete,false);
d=registry.ingest(packet('Headphones',4,true));assert.equal(d.name,'Headphones');assert.equal(d.source,'Scan response');assert.equal(d.complete,true);
registry.ingest(p);assert.equal(d.name,'Headphones');registry.ingest(packet('',0,true));assert.equal(d.name,'Headphones');assert.equal(d.count,4);
assert.equal(registry.lookup(d.address,true),d);
registry.ingest(packet('Public'));assert.equal(registry.lookup(d.address),null);assert.equal(registry.devices.size,2);
assert.equal(registry.lookup(d.address,false).name,'Public');
assert.equal(advertisement(packet('温度計')).name,'温度計');
assert.equal(advertisement({...p,access_address:'12345678'}),null);
assert.equal(advertisement({...p,pdu:p.pdu+'00'}),null);
assert.equal(advertisement({...p,pdu:'00080102030405060509'}),null);
assert.equal(advertisement(packet('Not a name',3)),null); // SCAN_REQ is not advertising data.
assert.equal(advertisement(packet('Not a name',5)),null); // CONNECT_IND is not advertising data.
assert.equal(advertisement(packet('Not a name',7)),null); // Extended data needs chain handling.
const capped=new DeviceRegistry(2);capped.ingest(packet('One'));capped.ingest(packet('Two',0,false,true,'020203040506'));capped.ingest(packet('Three',0,false,true,'030203040506'));assert.equal(capped.devices.size,2);assert.equal(capped.lookup('06:05:04:03:02:01'),null);
assert.equal(new DeviceRegistry().devices.size,0);
console.log('Advertising names, scan responses, address identity, malformed input and retention tests passed');
const rejectedNames=new DeviceRegistry();
assert.equal(rejectedNames.ingest({...packet('Corrupt name'),crc_valid:false}),null);
assert.equal(rejectedNames.devices.size,0);
