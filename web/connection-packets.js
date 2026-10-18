// SPDX-License-Identifier: MIT
// Virtual rows retain every packet while empty runs can be expanded in place.
class ConnectionPacketView {
  constructor() {
    this.$=id=>document.getElementById(id);
    this.height=52;this.id=null;this.revision=-1;this.rows=[];this.expanded=new Set();
    this.scroll=this.$('connection-packet-scroll');
    this.scroll.addEventListener('scroll',()=>this.render());
    // Stop rows moving under the pointer before a user can choose a packet.
    this.scroll.addEventListener('pointerenter',()=>{if(this.visiblePackets){this.hold();this.$('connection-packets-summary').textContent=`${this.visiblePackets.length.toLocaleString()} retained packets · View held; capture continues`;}});
    this.scroll.addEventListener('focusin',e=>{if(e.target.closest('button'))this.hold();});
    // Do not replace a pressed row between pointerdown and click.
    this.scroll.addEventListener('pointerdown',()=>this.interacting=true);
    const release=()=>setTimeout(()=>{this.interacting=false;this.render();},100);
    window.addEventListener('pointerup',release);window.addEventListener('pointercancel',release);
    this.$('group-empty').onchange=()=>{this.revision=-1;if(this.state)this.update(this.state);};
    this.$('assume-plaintext').onchange=()=>{this.render();if(this.selectedPacket&&!this.$('packet-detail').hidden)this.detail(this.selectedPacket);};
    this.$('connection-packet-follow').onchange=()=>{if(this.$('connection-packet-follow').checked){this.frozenPackets=null;this.scroll.scrollTop=0;}else this.hold();this.revision=-1;this.update(this.state);};
    this.$('close-packet-detail').onclick=()=>this.$('packet-detail').hidden=true;
  }
  node(tag,text,css) {const element=document.createElement(tag);if(text!=null)element.textContent=text;if(css)element.className=css;return element;}
  hex(value) {return value.match(/.{1,2}/g)?.join(' ')||'—';}
  signal(packet) {return Number.isFinite(packet.signal_dbfs)?packet.signal_dbfs.toFixed(1):'—';}
  noise(packet) {return Number.isFinite(packet.noise_dbfs)?packet.noise_dbfs.toFixed(1):'—';}
  noiseRange(packets) {
    let low=Infinity,high=-Infinity;
    for(const p of packets)if(Number.isFinite(p.noise_dbfs)){low=Math.min(low,p.noise_dbfs);high=Math.max(high,p.noise_dbfs);}
    return Number.isFinite(low)?(low===high?low.toFixed(1):low.toFixed(1)+'…'+high.toFixed(1)):'—';
  }
  hold() {this.$('connection-packet-follow').checked=false;this.state.autoSelect=false;if(!this.frozenPackets)this.frozenPackets=this.visiblePackets.slice();}
  update(state) {
    this.state=state;
    const connection=state.connections.get(state.selected);
    this.$('connection-packets-panel').hidden=!connection;
    if(!connection)return;
    if(this.id!==connection.id||this.session!==state.session) {
      this.id=connection.id;this.session=state.session;this.expanded.clear();this.revision=-1;
      this.scroll.scrollTop=0;this.frozenPackets=null;this.selectedPacket=null;this.$('assume-plaintext').checked=false;this.$('connection-packet-follow').checked=true;this.$('packet-detail').hidden=true;
    }
    if(this.frozenPackets&&this.revision!==-1)return;
    if(this.revision===state.revision)return;
    this.revision=state.revision;
    this.$('connection-packets-aa').textContent=connection.access_address;
    const retained=this.frozenPackets||state.packets.filter(p=>p.connection_id===this.id);
    this.visiblePackets=retained;
    const packets=retained.filter(p=>state.showCrcFailures!==false||p.crc_valid!==false);
    const dataCount=retained.filter(p=>p.access_address!=='8e89bed6'&&p.crc_valid!==false).length;
    const expired=Math.max(0,connection.count-dataCount);
    this.$('connection-packets-summary').textContent=`${packets.length.toLocaleString()} retained packets${this.frozenPackets?' · View held; capture continues':expired?' · '+expired.toLocaleString()+' earlier data packets expired':''}`;
    const anchor=this.rows[Math.floor(this.scroll.scrollTop/this.height)]?.key,offset=this.scroll.scrollTop%this.height;
    const groups=this.$('group-empty').checked?PacketModel.group(packets,state.gaps):packets.map(p=>({key:'g'+p.id,empty:false,packets:[p]}));
    if(this.selectedPacket?.crc_valid===false&&state.showCrcFailures===false)this.$('packet-detail').hidden=true;
    this.rows=[];
    for(const group of groups.reverse()) {
      if(group.empty&&group.packets.length>1) {
        this.rows.push({key:group.key,group});
        if(this.expanded.has(group.key))for(const p of group.packets.slice().reverse())this.rows.push({key:'p'+p.id,packet:p,child:true});
      } else this.rows.push({key:'p'+group.packets[0].id,packet:group.packets[0]});
    }
    this.$('connection-packet-spacer').style.height=this.rows.length*this.height+'px';
    if(this.$('connection-packet-follow').checked)this.scroll.scrollTop=0;
    else if(anchor){const index=this.rows.findIndex(r=>r.key===anchor);if(index>=0)this.scroll.scrollTop=index*this.height+offset;}
    this.$('connection-packets-empty').hidden=packets.length>0;
    this.render();
  }
  render() {
    if(this.interacting)return;
    const start=Math.max(0,Math.floor(this.scroll.scrollTop/this.height)-2),count=Math.ceil(this.scroll.clientHeight/this.height)+5;
    const fragment=document.createDocumentFragment();
    for(const row of this.rows.slice(start,start+count)) {
      const button=this.node('button',null,'connection-packet-row'+(row.child?' expanded-packet':'')+(row.packet?.crc_valid===false?' crc-failed':''));
      button.type='button';
      if(row.group) {
        const group=row.group,packets=group.packets,first=packets[0],last=packets[packets.length-1];
        const channels=new Set(packets.map(p=>p.channel));let low=Infinity,high=-Infinity;
        for(const p of packets)if(Number.isFinite(p.signal_dbfs)){low=Math.min(low,p.signal_dbfs);high=Math.max(high,p.signal_dbfs);}
        const open=this.expanded.has(group.key);
        const time=this.node('span',last.capture_s.toFixed(6),'mono');time.append(this.node('small','from '+first.capture_s.toFixed(6)));
        button.append(time,this.node('span',channels.size===1?'CH '+last.channel:channels.size+' ch','mono'),
          this.node('span',(open?'▾ ':'▸ ')+packets.length.toLocaleString()+' empty','empty-run'),
          this.node('span',Number.isFinite(low)?low.toFixed(1)+'…'+high.toFixed(1):'—','mono signal-level'),
          this.node('span',this.noiseRange(packets),'mono noise-level'),
          this.node('span','No payload · '+((last.capture_s-first.capture_s)*1000).toFixed(1)+' ms · '+(open?'Collapse':'Expand every packet'),'packet-data'));
        button.setAttribute('aria-expanded',String(open));
        button.onclick=()=>{this.hold();if(open)this.expanded.delete(group.key);else this.expanded.add(group.key);this.revision=-1;this.update(this.state);};
      } else {
        const p=row.packet,d=PacketModel.describe(p),decoded=this.decode(p);
        const data=this.node('span',null,'packet-data');
        data.append(this.node('b',d.empty?`No payload · SN ${d.sn} · NESN ${d.nesn}`:decoded.summary,'decoded-summary'));
        if(!d.empty)data.append(this.node('small',this.hex(d.payload),'raw-preview'));
        button.append(this.node('span',p.capture_s.toFixed(6),'mono'),this.node('span','CH '+p.channel,'mono'),
          this.node('span',d.empty?'Empty':decoded.protocol),this.node('span',this.signal(p),'mono signal-level'),this.node('span',this.noise(p),'mono noise-level'),data);
        button.title=`Packet #${p.id} · ${decoded.summary} · ${d.length} payload bytes · click for decoded fields`;
        button.onclick=()=>{this.hold();this.revision=-1;this.update(this.state);this.detail(p);};
      }
      fragment.append(button);
    }
    const target=this.$('connection-packet-rows');target.style.transform=`translateY(${start*this.height}px)`;target.replaceChildren(fragment);
  }
  decode(packet) {return PacketModel.decode(packet,this.state.connections.get(this.id),this.state.gaps,this.$('assume-plaintext').checked);}
  detail(packet) {
    this.selectedPacket=packet;
    const d=PacketModel.describe(packet),decoded=this.decode(packet),box=this.$('packet-detail-content');box.replaceChildren();
    this.$('packet-detail').hidden=false;
    box.append(this.node('h3',`Packet #${packet.id} · ${d.kind}`));
    const facts=this.node('div',null,'packet-detail-facts');
    for(const [label,value] of [['Capture time',packet.capture_s.toFixed(6)+' s'],['Channel','CH '+packet.channel],
      ['RSSI / signal',this.signal(packet)+' dBFS'],['Noise floor estimate',Number.isFinite(packet.noise_dbfs)?this.noise(packet)+' dBFS':'Unavailable'],['Payload length',d.length+' bytes'],['CRC status',packet.crc_valid===false?'FAILED — unverified bytes':'Valid'],['Received CRC',packet.crc],...packet.crc_valid===false?[['Expected CRC',packet.expected_crc||'Unavailable']]:[],['Access address',packet.access_address],
      ...packet.access_address!=='8e89bed6'?[['LLID',d.llid],['SN / NESN / MD',`${d.sn} / ${d.nesn} / ${d.md}`]]:[]]) {
      const fact=this.node('div');fact.append(this.node('span',label),this.node('b',value));facts.append(fact);
    }
    box.append(facts);
    const interpretation=this.node('section',null,'packet-interpretation');
    interpretation.append(this.node('h4',decoded.protocol+' · '+decoded.name));
    for(const warning of decoded.warnings)interpretation.append(this.node('p',warning,'decode-warning'));
    const fields=this.node('dl',null,'decoded-fields');
    for(const [label,value] of decoded.fields)if(!['LLID','SN / NESN / MD','Payload length'].includes(label)){
      fields.append(this.node('dt',label),this.node('dd',value));
    }
    interpretation.append(fields);box.append(interpretation);
    box.append(this.node('h4','Captured payload'),this.node('pre',d.empty?'Empty payload':this.hex(d.payload)),
      this.node('h4','Complete PDU · header + payload'),this.node('pre',this.hex(packet.pdu)),
      this.node('p','Captured bytes are shown as received after dewhitening. Encrypted payloads remain ciphertext. Signal and noise are channel-filtered power in dBFS, not calibrated dBm. Noise uses a nearby 32 µs window before synchronization, excluding recognized packets; it may include interference. A dash means no complete window is available.','packet-detail-note'));
  }
}
