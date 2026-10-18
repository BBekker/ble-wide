const assert=require('node:assert/strict');
const {advertisement,DeviceRegistry}=require('../../web/packet-model.js');
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

const {DeviceFilter}=require('../../web/packet-model.js');
const publicKey='public:06:05:04:03:02:01',randomKey='random:06:05:04:03:02:01';
const filter=new DeviceFilter([publicKey,'invalid']);
const connections=new Map([['a',{central:'06:05:04:03:02:01',peripheral:'16:15:14:13:12:11'}],['b',{central:'ff:ff:ff:ff:ff:ff',peripheral:'16:15:14:13:12:11'}],['unknown',{central:'',peripheral:''}]]);
assert.equal(filter.selected.size,1);
assert.equal(filter.matchesPacket(packet('Public'),connections),true);
assert.equal(filter.matchesPacket(packet('Random',0,true),connections),false);
assert.equal(filter.matchesPacket(packet('Reply',4),connections),true);
assert.equal(filter.matchesPacket({...packet('Damaged'),crc_valid:false},connections),true);
assert.equal(filter.matchesPacket({access_address:'12345678',connection_id:'a'},connections),true);
assert.equal(filter.matchesPacket({access_address:'12345678',connection_id:'b'},connections),false);
assert.equal(filter.matchesPacket({access_address:'12345678',connection_id:'unknown'},connections),false);
assert.equal(filter.matchesPacket({access_address:'12345678'},connections),false);
assert.equal(filter.matchesConnection({central:'06:05:04:03:02:01',central_random:true}),false);
assert.equal(filter.matchesConnection({peripheral:'06:05:04:03:02:01',peripheral_random:false}),true);
// CONNECT_IND and SCAN_REQ identify both endpoints; do not match only the sender.
for(const type of [3,5]) {
 const body=Buffer.concat([Buffer.from('111213141516010203040506','hex'),Buffer.alloc(type===5?22:0)]);
 const p={access_address:'8e89bed6',pdu:Buffer.concat([Buffer.from([type,body.length]),body]).toString('hex')};
 assert.equal(filter.matchesPacket(p,connections),true);
 assert.equal(filter.matchesPacket({...p,pdu:p.pdu+'00'},connections),false);
}
const directed={access_address:'8e89bed6',device_key:'public:16:15:14:13:12:11',pdu:'010c111213141516010203040506'};
assert.equal(filter.matchesPacket(directed,connections),true);
filter.selected.add('public:16:15:14:13:12:11');
assert.equal(filter.matchesPacket({access_address:'12345678',connection_id:'b'},connections),true);
filter.selected.clear();
assert.equal(filter.matchesPacket({access_address:'12345678',connection_id:'unknown'},connections),true);
// Reused access addresses must match the connection ID, not the radio AA.
filter.selected.add(publicKey);
assert.equal(filter.matchesPacket({access_address:'same',connection_id:'a'},connections),true);
assert.equal(filter.matchesPacket({access_address:'same',connection_id:'b'},connections),false);
console.log('Device selection: multi-select, both endpoints, scan responses, CRC failures, unknown connections and reset passed');
