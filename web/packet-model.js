// SPDX-License-Identifier: MIT
(function(root) {
  function describe(packet) {
    const hex=packet.pdu||'', header=parseInt(hex.slice(0,2),16), length=parseInt(hex.slice(2,4),16);
    const advertising=packet.access_address==='8e89bed6', llid=header&3;
    const empty=packet.crc_valid!==false&&!advertising&&llid===1&&length===0&&hex.length===4;
    return {empty,header,length,llid,nesn:(header>>2)&1,sn:(header>>3)&1,md:(header>>4)&1,
      payload:hex.slice(4),kind:packet.crc_valid===false?'CRC failed':advertising?packet.kind||'Advertising':empty?'Empty':llid===3?'Control':llid===2?'Data start':'Continuation'};
  }
  function group(packets,gaps=[]) {
    const groups=[];let previous=null;
    for(const packet of packets) {
      const empty=describe(packet).empty, last=groups[groups.length-1];
      const interrupted=previous&&(previous.epoch!==packet.epoch||gaps.some(g=>g.end>previous.capture_s&&g.start<packet.capture_s));
      if(empty&&last&&last.empty&&!interrupted)last.packets.push(packet);
      else groups.push({key:'g'+packet.id,empty,packets:[packet]});
      previous=packet;
    }
    return groups;
  }
  // Bluetooth Core 6.1: Vol 6 Part B 2.4; Vol 3 Parts A and F.
  const hex = (n,width=2) => '0x'+n.toString(16).padStart(width,'0');
  const bytesHex = bytes => Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join(' ');
  const u16 = (b,i) => b[i]|b[i+1]<<8;
  const signed = n => n&128?n-256:n;
  const uuid = b => b.length===2?hex(u16(b,0),4):Array.from(b).reverse().map(v=>v.toString(16).padStart(2,'0')).join('').replace(/^(.{8})(.{4})(.{4})(.{4})(.{12})$/,'$1-$2-$3-$4-$5');
  const result = (protocol,name) => ({protocol,name,summary:name,fields:[],warnings:[],status:'decoded'});
  const field = (r,name,value) => r.fields.push([name,String(value)]);
  function need(b,n,exact=false) {if(b.length<n||(exact&&b.length!==n))throw new Error(`Expected ${exact?'exactly ':'at least '}${n} bytes; received ${b.length}.`);}
  function value(r,label,b) {field(r,label,bytesHex(b)||'(empty)');if(b.length&&b.every(x=>x>=32&&x<=126))field(r,label+' · text',String.fromCharCode(...b));}
  const LL_NAMES=['CONNECTION_UPDATE_IND','CHANNEL_MAP_IND','TERMINATE_IND','ENC_REQ','ENC_RSP','START_ENC_REQ','START_ENC_RSP','UNKNOWN_RSP','FEATURE_REQ','FEATURE_RSP','PAUSE_ENC_REQ','PAUSE_ENC_RSP','VERSION_IND','REJECT_IND','PERIPHERAL_FEATURE_REQ','CONNECTION_PARAM_REQ','CONNECTION_PARAM_RSP','REJECT_EXT_IND','PING_REQ','PING_RSP','LENGTH_REQ','LENGTH_RSP','PHY_REQ','PHY_RSP','PHY_UPDATE_IND','MIN_USED_CHANNELS_IND','CTE_REQ','CTE_RSP','PERIODIC_SYNC_IND','CLOCK_ACCURACY_REQ','CLOCK_ACCURACY_RSP','CIS_REQ','CIS_RSP','CIS_IND','CIS_TERMINATE_IND','POWER_CONTROL_REQ','POWER_CONTROL_RSP','POWER_CHANGE_IND','SUBRATE_REQ','SUBRATE_IND','CHANNEL_REPORTING_IND','CHANNEL_STATUS_IND'];
  const LL_LENGTHS={0:12,1:8,2:2,3:23,4:13,5:1,6:1,7:2,8:9,9:9,10:1,11:1,12:6,13:2,14:9,15:24,16:24,17:3,18:1,19:1,20:9,21:9,22:3,23:3,24:5,25:3,26:2,27:1,29:2,30:2,34:4,35:4,36:5,37:5,38:11,39:11,40:4,41:11};
  const phy = n => [n&1?'LE 1M':'',n&2?'LE 2M':'',n&4?'LE Coded':''].filter(Boolean).join(', ')||'No preference';
  const txPower = n => n===127?'Unavailable':n===126?'Not managed':signed(n)+' dBm';
  function control(b) {
    need(b,1);const op=b[0],r=result('LL control',LL_NAMES[op]?'LL_'+LL_NAMES[op]:'Unknown LL opcode '+hex(op));
    field(r,'Opcode',hex(op));if(op in LL_LENGTHS)need(b,LL_LENGTHS[op],true);
    switch(op) {
      case 0: field(r,'Transmit window',b[1]*1.25+' ms');field(r,'Window offset',u16(b,2)*1.25+' ms');field(r,'Connection interval',u16(b,4)*1.25+' ms');field(r,'Peripheral latency',u16(b,6)+' events');field(r,'Supervision timeout',u16(b,8)*10+' ms');field(r,'Instant',u16(b,10));r.summary+=' · '+u16(b,4)*1.25+' ms';break;
      case 1: {const enabled=Array.from({length:37},(_,ch)=>ch).filter(ch=>b[1+(ch>>3)]&(1<<(ch&7)));field(r,'Enabled channels',enabled.join(', '));field(r,'Channel map',bytesHex(b.slice(1,6)));field(r,'Instant',u16(b,6));r.summary+=' · '+enabled.length+' channels';break;}
      case 2: case 13: field(r,'Error / reason',hex(b[1]));break;
      case 3: value(r,'Random number',b.slice(1,9));field(r,'Encrypted diversifier',hex(u16(b,9),4));value(r,'Central SKD',b.slice(11,19));value(r,'Central IV',b.slice(19,23));break;
      case 4: value(r,'Peripheral SKD',b.slice(1,9));value(r,'Peripheral IV',b.slice(9,13));break;
      case 7: field(r,'Unknown opcode',hex(b[1]));break;
      case 8: case 9: case 14: value(r,'Feature bitmask',b.slice(1));field(r,'Enabled feature bits',Array.from({length:64},(_,i)=>i).filter(i=>b[1+(i>>3)]&(1<<(i&7))).join(', '));break;
      case 12: field(r,'Version number',hex(b[1]));field(r,'Company identifier',hex(u16(b,2),4));field(r,'Subversion',hex(u16(b,4),4));break;
      case 15: case 16: field(r,'Interval range',u16(b,1)*1.25+'–'+u16(b,3)*1.25+' ms');field(r,'Latency',u16(b,5));field(r,'Timeout',u16(b,7)*10+' ms');field(r,'Preferred periodicity',b[9]);field(r,'Reference event',u16(b,10));field(r,'Offsets',Array.from({length:6},(_,i)=>u16(b,12+i*2)===65535?'Unspecified':u16(b,12+i*2)*1.25+' ms').join(', '));break;
      case 17: field(r,'Rejected opcode',hex(b[1]));field(r,'Error code',hex(b[2]));break;
      case 20: case 21: field(r,'Max RX octets',u16(b,1));field(r,'Max RX time',u16(b,3)+' µs');field(r,'Max TX octets',u16(b,5));field(r,'Max TX time',u16(b,7)+' µs');break;
      case 22: case 23: field(r,'TX PHYs',phy(b[1]));field(r,'RX PHYs',phy(b[2]));break;
      case 24: field(r,'Central → peripheral PHY',['No change','LE 1M','LE 2M','LE Coded'][b[1]]||'Reserved');field(r,'Peripheral → central PHY',['No change','LE 1M','LE 2M','LE Coded'][b[2]]||'Reserved');field(r,'Instant',u16(b,3));break;
      case 25: field(r,'PHYs',phy(b[1]));field(r,'Minimum used channels',b[2]);break;
      case 26: field(r,'Minimum CTE length',(b[1]&31)*8+' µs');field(r,'CTE type',b[1]>>6);break;
      case 29: case 30: field(r,'Sleep clock accuracy class',b[1]);break;
      case 34: field(r,'CIG ID',b[1]);field(r,'CIS ID',b[2]);field(r,'Reason',hex(b[3]));break;
      case 35: field(r,'PHY mask',hex(b[1]));field(r,'Requested power change',b[2]===127?'Maximum power':signed(b[2])+' dB');field(r,'Sender TX power',txPower(b[3]));break;
      case 36: field(r,'Power limits',(b[1]&1?'Minimum ':'')+(b[1]&2?'Maximum':'')||'Neither');field(r,'Actual power change',signed(b[2])+' dB');field(r,'Sender TX power',txPower(b[3]));field(r,'Acceptable reduction',b[4]===255?'Unknown':b[4]+' dB');break;
      case 37: field(r,'PHY mask',hex(b[1]));field(r,'Power limit flags',hex(b[2]));field(r,'Power change',signed(b[3])+' dB');field(r,'Sender TX power',txPower(b[4]));break;
      case 38: case 39: {const names=op===38?['Minimum subrate','Maximum subrate','Maximum latency','Continuation number','Supervision timeout']:['Subrate factor','Base event','Peripheral latency','Continuation number','Supervision timeout'];names.forEach((n,i)=>field(r,n,u16(b,1+2*i)+(i===4?' × 10 ms':'')));break;}
      case 40: field(r,'Reporting',b[1]?'Enabled':'Disabled');field(r,'Minimum spacing',b[2]*200+' ms');field(r,'Maximum delay',b[3]*200+' ms');break;
      case 41: for(const [code,label] of [[0,'Unknown channels'],[1,'Good channels'],[3,'Bad channels']])field(r,label,Array.from({length:37},(_,ch)=>ch).filter(ch=>((b[1+(ch>>2)]>>((ch&3)*2))&3)===code).join(', ')||'None');break;
      default: if(b.length>1){value(r,'Control data',b.slice(1));r.warnings.push('Fields for this control opcode are not yet decoded.');}
    }
    return r;
  }
  const ATT_NAMES={1:'Error response',2:'Exchange MTU request',3:'Exchange MTU response',4:'Find information request',5:'Find information response',6:'Find by type value request',7:'Find by type value response',8:'Read by type request',9:'Read by type response',10:'Read request',11:'Read response',12:'Read blob request',13:'Read blob response',14:'Read multiple request',15:'Read multiple response',16:'Read by group type request',17:'Read by group type response',18:'Write request',19:'Write response',22:'Prepare write request',23:'Prepare write response',24:'Execute write request',25:'Execute write response',27:'Notification',29:'Indication',30:'Confirmation',32:'Read multiple variable request',33:'Read multiple variable response',35:'Multiple notifications',82:'Write command',210:'Signed write command'};
  const ATT_ERRORS={1:'Invalid handle',2:'Read not permitted',3:'Write not permitted',4:'Invalid PDU',5:'Insufficient authentication',6:'Request not supported',7:'Invalid offset',8:'Insufficient authorization',9:'Prepare queue full',10:'Attribute not found',11:'Attribute not long',12:'Encryption key size too short',13:'Invalid attribute value length',14:'Unlikely error',15:'Insufficient encryption',16:'Unsupported group type',17:'Insufficient resources',18:'Database out of sync',19:'Value not allowed'};
  function att(b) {
    need(b,1);const op=b[0],r=result('ATT',ATT_NAMES[op]||'Unknown ATT opcode '+hex(op));r.summary='ATT '+r.name;field(r,'Opcode',hex(op));
    const handle=i=>hex(u16(b,i),4),range=()=>{field(r,'Starting handle',handle(1));field(r,'Ending handle',handle(3));};
    switch(op) {
      case 1: need(b,5,true);field(r,'Request in error',ATT_NAMES[b[1]]||hex(b[1]));field(r,'Handle',handle(2));field(r,'Error',(ATT_ERRORS[b[4]]||'Unrecognized error')+' ('+hex(b[4])+')');r.summary+=' · '+(ATT_ERRORS[b[4]]||hex(b[4]));break;
      case 2: case 3: need(b,3,true);field(r,'Receive MTU',u16(b,1)+' bytes');r.summary+=' · '+u16(b,1)+' bytes';break;
      case 4: need(b,5,true);range();break;
      case 5: {need(b,2);const stride=b[1]===1?4:b[1]===2?18:0;if(!stride||(b.length-2)%stride||b.length===2)throw new Error('Invalid handle/UUID list.');for(let i=2;i<b.length;i+=stride)field(r,'Handle '+handle(i),'UUID '+uuid(b.slice(i+2,i+stride)));break;}
      case 6: need(b,7);range();field(r,'Attribute type',handle(5));value(r,'Value',b.slice(7));break;
      case 7: need(b,5);if((b.length-1)%4)throw new Error('Incomplete handle range.');for(let i=1;i<b.length;i+=4)field(r,'Handle range',handle(i)+'–'+handle(i+2));break;
      case 8: case 16: if(![7,21].includes(b.length))throw new Error('Expected a 16-bit or 128-bit UUID.');range();field(r,'Attribute type UUID',uuid(b.slice(5)));break;
      case 9: case 17: {need(b,2);const size=b[1],minimum=op===9?2:4;if(size<minimum||b.length===2||(b.length-2)%size)throw new Error('Incomplete attribute data list.');for(let i=2;i<b.length;i+=size){field(r,op===9?'Handle '+handle(i):'Group '+handle(i)+'–'+handle(i+2),bytesHex(b.slice(i+minimum,i+size))||'(empty)');}break;}
      case 10: need(b,3,true);field(r,'Handle',handle(1));r.summary+=' · '+handle(1);break;
      case 12: need(b,5,true);field(r,'Handle',handle(1));field(r,'Value offset',u16(b,3));break;
      case 11: case 13: case 15: value(r,'Value',b.slice(1));break;
      case 14: case 32: need(b,5);if((b.length-1)%2)throw new Error('Incomplete handle list.');field(r,'Handles',Array.from({length:(b.length-1)/2},(_,i)=>handle(1+2*i)).join(', '));break;
      case 18: case 82: case 27: case 29: need(b,3);field(r,'Handle',handle(1));value(r,'Value',b.slice(3));r.summary+=' · '+handle(1)+' · '+(b.length-3)+' bytes';break;
      case 210: need(b,15);field(r,'Handle',handle(1));value(r,'Value',b.slice(3,-12));value(r,'Authentication signature',b.slice(-12));r.warnings.push('Signature is displayed, not verified.');break;
      case 19: case 25: case 30: need(b,1,true);break;
      case 22: case 23: need(b,5);field(r,'Handle',handle(1));field(r,'Value offset',u16(b,3));value(r,'Value',b.slice(5));break;
      case 24: need(b,2,true);field(r,'Action',b[1]===0?'Cancel prepared writes':b[1]===1?'Execute prepared writes':'Reserved '+hex(b[1]));break;
      case 33: case 35: {let i=1,n=0;while(i<b.length){need(b,i+(op===35?4:2));const h=op===35?handle(i):'Value '+(++n);if(op===35)i+=2;const size=u16(b,i);i+=2;need(b,i+size);value(r,h,b.slice(i,i+size));i+=size;}if(i===1)throw new Error('Empty value list.');break;}
      default: value(r,'Parameters',b.slice(1));r.status='unsupported';r.warnings.push('Unknown ATT opcode; parameters remain raw.');
    }
    return r;
  }
  function signaling(b) {
    const r=result('L2CAP signaling','LE signaling');let i=0;
    while(i<b.length) {
      need(b,i+4);const code=b[i],id=b[i+1],len=u16(b,i+2);need(b,i+4+len);const p=b.slice(i+4,i+4+len);
      const names={1:'Command reject',6:'Disconnection request',7:'Disconnection response',18:'Connection parameter update request',19:'Connection parameter update response',20:'LE credit connection request',21:'LE credit connection response',22:'Flow control credit'};
      const name=names[code]||'Command '+hex(code);field(r,name,'Identifier '+id);r.summary='L2CAP '+name;
      if(code===18){need(p,8,true);field(r,'Interval range',u16(p,0)*1.25+'–'+u16(p,2)*1.25+' ms');field(r,'Latency',u16(p,4));field(r,'Timeout',u16(p,6)*10+' ms');}
      else if(code===19){need(p,2,true);field(r,'Result',u16(p,0)===0?'Accepted':u16(p,0)===1?'Rejected':hex(u16(p,0),4));}
      else if(code===6||code===7){need(p,4,true);field(r,'Destination CID',hex(u16(p,0),4));field(r,'Source CID',hex(u16(p,2),4));}
      else if(code===20||code===21){need(p,10,true);const fields=code===20?['LE PSM','Source CID','MTU','MPS','Initial credits']:['Destination CID','MTU','MPS','Initial credits','Result'];fields.forEach((n,k)=>field(r,n,u16(p,k*2)));}
      else if(code===22){need(p,4,true);field(r,'CID',hex(u16(p,0),4));field(r,'Credits',u16(p,2));}
      else value(r,'Command data',p);
      i+=4+len;
    }
    return r;
  }
  function encryptionState(packet,connection,gaps) {
    if(packet.encrypted===true)return 'encrypted';
    const markers=(connection?.markers||[]).filter(m=>m.kind==='encryption').sort((a,b)=>a.time-b.time);
    if(markers.some(m=>packet.capture_s>m.time||packet.id>m.id))return 'encrypted';
    if(connection?.encrypted&&!markers.length)return 'encrypted';
    if(!connection?.observed)return 'unknown';
    if(gaps.some(g=>g.start>=connection.start&&g.start<packet.capture_s))return 'unknown';
    if(connection.warning?.includes('Input gap'))return 'unknown';
    return 'plaintext';
  }
  function decode(packet,connection=null,gaps=[],assumePlaintext=false) {
    const r=result('Link layer','Data PDU'),text=packet.pdu||'';
    if(packet.crc_valid===false){r.protocol='CRC failed';r.name=r.summary='Unverified packet bytes';r.status='crc_failed';r.warnings.push('CRC mismatch: bytes may be corrupt. This packet does not update connection state.');return r;}
    try {
      if(!/^(?:[0-9a-f]{2})+$/i.test(text))throw new Error('Invalid PDU hex bytes.');
      const b=Uint8Array.from(text.match(/../g),v=>parseInt(v,16));need(b,2);
      const d=describe(packet);field(r,'LLID',d.llid);field(r,'SN / NESN / MD',`${d.sn} / ${d.nesn} / ${d.md}`);field(r,'Payload length',d.length+' bytes');
      if(packet.access_address==='8e89bed6'){r.name=r.summary=packet.kind||'Advertising PDU';r.status='unsupported';return r;}
      if(b[0]&0xe0)throw new Error('CTEInfo/reserved header bits are not supported by this capture format.');
      if(d.llid===0)throw new Error('Reserved LLID.');need(b,d.length+2,true);
      if(d.empty){r.name=r.summary='Empty data PDU';return r;}
      const security=encryptionState(packet,connection,gaps);
      if(security==='encrypted'){r.name=r.summary='Encrypted payload';r.status='encrypted';r.warnings.push('Encryption was observed. Payload cannot be decoded without session keys.');return r;}
      if(security==='unknown'&&!assumePlaintext){r.name=r.summary='Encryption state unknown';r.status='unknown';r.warnings.push('Setup or control traffic is missing. Enable “Assume plaintext” only if this connection is unencrypted.');return r;}
      if(security==='unknown')r.warnings.push('Assumed plaintext: encryption state could not be verified.');
      const payload=b.slice(2);let parsed;
      if(d.llid===3)parsed=control(payload);
      else if(d.llid===1){r.name=r.summary='L2CAP continuation fragment';r.status='fragment';r.warnings.push('Not reassembled: transmitter direction and a complete fragment sequence are required.');return r;}
      else {
        need(payload,4);const size=u16(payload,0),cid=u16(payload,2),body=payload.slice(4);
        field(r,'L2CAP payload length',size+' bytes');field(r,'L2CAP channel ID',hex(cid,4));
        if(body.length<size){r.name=r.summary=`L2CAP start fragment · ${body.length}/${size} bytes`;r.status='fragment';r.warnings.push('The complete L2CAP message is not contained in this packet.');return r;}
        if(body.length!==size)throw new Error('L2CAP length does not match the captured payload.');
        if(cid===4)parsed=att(body);
        else if(cid===5)parsed=signaling(body);
        else {parsed=result('L2CAP',cid===6?'Security Manager payload':'L2CAP channel '+hex(cid,4));parsed.status='unsupported';value(parsed,'Channel payload',body);parsed.warnings.push('This channel protocol is not yet decoded.');}
      }
      return {...parsed,fields:r.fields.concat(parsed.fields),warnings:r.warnings.concat(parsed.warnings)};
    } catch(error) {r.name=r.summary='Malformed or truncated PDU';r.status='malformed';r.warnings.push(error.message);return r;}
  }

  // Local names are AD types 0x08 (shortened) and 0x09 (complete).
  // Address type is part of identity; a rotating random address is a new record.
  function advertisement(packet) {
    if(packet.access_address!=='8e89bed6'||!/^(?:[0-9a-f]{2})+$/i.test(packet.pdu||''))return null;
    const b=Uint8Array.from(packet.pdu.match(/../g),v=>parseInt(v,16));
    if(b.length<8||b.length>39||b.length!==b[1]+2)return null;
    const type=b[0]&15;let offset=8;
    if(![0,1,2,4,6].includes(type))return null;
    if(type===1&&b.length!==14)return null;
    const address=Array.from(b.slice(2,8)).reverse().map(v=>v.toString(16).padStart(2,'0')).join(':');
    const result={address,random:!!(b[0]&64),name:'',complete:false,source:type===4?'Scan response':'Advertisement'};
    if(type===1)return result;
    while(offset<b.length){
      const n=b[offset++];if(!n)break;
      if(offset+n>b.length)return null;
      const ad=b[offset];
      if((ad===8||ad===9)&&n>1){
        let name;try{name=new TextDecoder('utf-8',{fatal:true}).decode(b.slice(offset+1,offset+n)).replace(/[\u0000-\u001f\u007f-\u009f]/g,'').trim();}catch{offset+=n;continue;}
        if(name&&(!result.name||ad===9)){result.name=name;result.complete=ad===9;}
      }
      offset+=n;
    }
    return result;
  }
  class DeviceRegistry {
    constructor(limit=10000){this.devices=new Map();this.limit=limit;this.revision=0;this.namesRevision=0;}
    ingest(packet){
      if(packet.crc_valid===false)return null;
      const ad=advertisement(packet);if(!ad)return null;
      const key=(ad.random?'random:':'public:')+ad.address;
      let d=this.devices.get(key);
      if(!d){d={...ad,key,name:'',complete:false,first:packet.capture_s,count:0,channels:new Set()};this.devices.set(key,d);}
      if(ad.name&&(!d.name||ad.complete||!d.complete&&ad.name.length>=d.name.length)){if(d.name!==ad.name)this.namesRevision++;d.name=ad.name;d.complete=ad.complete;d.source=ad.source;}
      d.last=packet.capture_s;d.count++;d.channels.add(packet.channel);
      if(Number.isFinite(packet.signal_dbfs))d.signal=packet.signal_dbfs;
      // Touch insertion order for bounded least-recently-seen eviction.
      this.devices.delete(key);this.devices.set(key,d);
      if(this.devices.size>this.limit)this.devices.delete(this.devices.keys().next().value);
      this.revision++;return d;
    }
    lookup(address,random){
      if(!address)return null;
      if(typeof random==='boolean')return this.devices.get((random?'random:':'public:')+address)||null;
      const a=this.devices.get('public:'+address),b=this.devices.get('random:'+address);
      return a&&b?null:a||b||null;
    }
  }

  // View-only filtering: retain every captured packet, match data by connection
  // identity and primary-channel traffic by the addresses carried in its PDU.
  class DeviceFilter {
    constructor(keys=[]) {
      this.selected=new Set(keys.filter(k=>/^(public|random):(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$/.test(k)));
      this.cache=new WeakMap();
    }
    matchesConnection(c) {
      if(!this.selected.size)return true;
      if(!c)return false;
      return ['central','peripheral'].some(role=>{
        const address=(c[role]||'').toLowerCase(),random=c[role+'_random'];
        if(!address)return false;
        return typeof random==='boolean'?this.selected.has((random?'random:':'public:')+address):
          this.selected.has('public:'+address)||this.selected.has('random:'+address);
      });
    }
    packetKeys(p) {
      if(p.device_key&&(parseInt(p.pdu?.slice(0,2),16)&15)!==1)return [p.device_key];
      if(this.cache.has(p))return this.cache.get(p);
      const keys=[];
      if(p.access_address==='8e89bed6'&&/^(?:[0-9a-f]{2})+$/i.test(p.pdu||'')) {
        const b=Uint8Array.from(p.pdu.match(/../g),v=>parseInt(v,16)),type=b[0]&15;
        const add=(offset,random)=>{if(b.length>=offset+6)keys.push((random?'random:':'public:')+Array.from(b.slice(offset,offset+6)).reverse().map(v=>v.toString(16).padStart(2,'0')).join(':'));};
        if(b.length>=8&&b.length===b[1]+2&&type<=6) {
          add(2,!!(b[0]&64));
          if([1,3,5].includes(type))add(8,!!(b[0]&128));
        }
      }
      this.cache.set(p,keys);return keys;
    }
    matchesPacket(p,connections) {
      if(!this.selected.size)return true;
      if(p.access_address!=='8e89bed6')return this.matchesConnection(connections.get(p.connection_id));
      return this.packetKeys(p).some(key=>this.selected.has(key));
    }
  }

  const api={describe,group,decode,encryptionState,advertisement,DeviceRegistry,DeviceFilter};
  if(typeof module==='object'&&module.exports)module.exports=api;else root.PacketModel=api;
})(typeof globalThis==='object'?globalThis:this);
