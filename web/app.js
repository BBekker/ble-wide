// SPDX-License-Identifier: MIT
// One bounded feed drives both plots. Canvas paints are independent of delivery;
// packets are never sampled out to obtain a frame rate.
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const ADV = '8e89bed6', LIMIT = 50000, ROW_HEIGHT = 40;
  const number = n => Number(n || 0).toLocaleString();
  const frequency = ch => ch===37?2402:ch===38?2426:ch===39?2480:ch<11?2404+2*ch:2406+2*ch;
  const order = Array.from({length:40}, (_,ch)=>ch).sort((a,b)=>frequency(a)-frequency(b));
  const column = new Map(order.map((ch,i)=>[ch,i]));
  const channelColor = ch => `hsl(${170+ch*4.5} 65% 68%)`;
  const node = (tag, value, css) => {const e=document.createElement(tag); if(value!=null)e.textContent=value;if(css)e.className=css;return e;};
  const setText = (id,value) => {if($(id).textContent!==String(value))$(id).textContent=value;};
  const seconds = n => n.toFixed(store.span<.01?6:3)+' s';
  const store = {session:null, cursor:0, spectrumCursor:0, packets:[], spectrum:[], connections:new Map(), byConnection:new Map(), gaps:[],
    active:false, connected:false, status:{}, clock:0, receivedAt:0, lastMessage:0, revision:0, selected:null, autoSelect:true,
    frozen:null, span:30, changed:true, filtered:[], filterChannel:null, filterQuery:'', filterKind:'', pausedList:false, listFrozen:[], showCrcFailures:true};
  try {store.showCrcFailures=localStorage.getItem('airspace-show-crc-failures')!=='false';} catch {}
  $('show-crc-failures').checked=store.showCrcFailures;
  const visiblePacket=p=>store.showCrcFailures||p.crc_valid!==false;
  let devices=new PacketModel.DeviceRegistry(),deviceRenderAt=-Infinity;
  const rowNodes = new Map();
  const connectionPackets = new ConnectionPacketView();
  const tooltip = $('tooltip');
  let lastUI=0,lastPaint=0, detailSignature='', previousRate=null, packetRate=0, pointerPoints=[];
  const water = $('waterfall'), raster=document.createElement('canvas'), rasterContext=raster.getContext('2d');
  let imageData=null;
  let autoLevels=new PlotMath.AutoLevels(), lastGainUpdate=-Infinity, lastGainEnd=null;
  const lut = Array.from({length:256},(_,i)=>{
    const stops=[[9,22,37],[17,43,70],[30,88,122],[38,145,155],[98,192,165],[240,218,128]];
    const x=i/255*(stops.length-1), k=Math.min(stops.length-2,Math.floor(x)), f=x-k;
    return stops[k].map((v,j)=>Math.round(v+(stops[k+1][j]-v)*f));
  });
  const clockNow = () => store.clock + (store.active && store.connected && store.frozen===null ? Math.min(.5, (performance.now()-store.receivedAt)/1000) : 0);
  function windowRange(){const end=store.frozen??clockNow();return {start:Math.max(0,end-store.span),end,span:Math.min(store.span,Math.max(.001,end))};}
  function earliest(){return Math.min(store.packets.length?store.packets[0].capture_s:Infinity,store.spectrum.length?store.spectrum[0].capture_s:Infinity,store.clock);}
  function syncNavigation(){
    const live=store.frozen===null, range=windowRange();
    $('timeline-freeze').textContent=live?'Ⅱ Pause':'▶ Resume';$('timeline-freeze').setAttribute('aria-pressed',String(!live));
    $('timeline-latest').classList.toggle('following',live);
    const preset=[...$('timeline-span').options].find(o=>Number(o.value)===store.span);
    if(preset)$('timeline-span').value=preset.value;
    else{const custom=$('timeline-span').querySelector('[value="custom"]');custom.textContent=store.span<1?(store.span*1000).toFixed(1)+' ms':store.span.toFixed(2)+' s';$('timeline-span').value='custom';}
    const history=Math.max(0,store.clock-earliest()-range.span);
    $('timeline-back').max=history;$('timeline-back').disabled=history<=0;
    $('timeline-back').value=Math.max(0,store.clock-range.end);
    store.changed=true;
  }
  function setView(end,span){const range=PlotMath.view(end,span,earliest(),clockNow());store.span=range.span;store.frozen=range.end;syncNavigation();tooltip.hidden=true;}
  function goLive(){store.frozen=null;syncNavigation();}
  function zoomAt(factor,anchor){const range=PlotMath.zoom(windowRange(),factor,anchor,earliest(),clockNow());setView(range.end,range.span);}
  function navigatePlot(canvas,vertical=false){
    const pointers=new Map();let drag=null,suppressClick=false;
    const coordinate=e=>vertical?e.clientY:e.clientX;
    const fraction=e=>{const rect=canvas.getBoundingClientRect();return vertical?1-(e.clientY-rect.top)/rect.height:(e.clientX-rect.left)/rect.width;};
    const size=()=>vertical?canvas.clientHeight:canvas.clientWidth;
    const direction=vertical?-1:1;
    function resetDrag(){
      const values=[...pointers.values()];if(!values.length){drag=null;return;}
      const range=windowRange();
      drag={range,origin:values.reduce((s,e)=>s+coordinate(e),0)/values.length,
        distance:values.length===2?Math.hypot(values[0].clientX-values[1].clientX,values[0].clientY-values[1].clientY):0,
        anchor:values.reduce((s,e)=>s+fraction(e),0)/values.length};
    }
    canvas.addEventListener('pointerdown',e=>{if(e.button!==0)return;pointers.set(e.pointerId,e);canvas.setPointerCapture(e.pointerId);suppressClick=false;resetDrag();});
    canvas.addEventListener('pointermove',e=>{
      if(!pointers.has(e.pointerId)||!drag)return;
      pointers.set(e.pointerId,e);const values=[...pointers.values()],center=values.reduce((s,e)=>s+coordinate(e),0)/values.length;
      if(values.length===1&&Math.abs(center-drag.origin)<3&&!suppressClick)return;
      suppressClick=true;canvas.classList.add('dragging');tooltip.hidden=true;
      let span=drag.range.span;
      if(values.length===2){const distance=Math.hypot(values[0].clientX-values[1].clientX,values[0].clientY-values[1].clientY);span=Math.max(.001,Math.min(120,drag.range.span*drag.distance/Math.max(1,distance)));}
      const anchorTime=drag.range.start+drag.anchor*drag.range.span;
      setView(anchorTime+(1-drag.anchor)*span-direction*(center-drag.origin)/size()*span,span);
      e.preventDefault();
    });
    function release(e){pointers.delete(e.pointerId);canvas.classList.remove('dragging');resetDrag();}
    canvas.addEventListener('pointerup',release);canvas.addEventListener('pointercancel',release);canvas.addEventListener('lostpointercapture',release);
    canvas.addEventListener('click',e=>{if(suppressClick){e.stopImmediatePropagation();suppressClick=false;}},true);
    canvas.addEventListener('wheel',e=>{
      e.preventDefault();tooltip.hidden=true;
      const scale=e.deltaMode===1?16:e.deltaMode===2?size():1;
      if(!e.ctrlKey&&Math.abs(e.deltaX)>Math.abs(e.deltaY)){const range=windowRange();setView(range.end+e.deltaX*scale/size()*range.span,range.span);}
      else zoomAt(Math.exp(Math.max(-300,Math.min(300,e.deltaY*scale))*(e.ctrlKey?.008:.003)),fraction(e));
    },{passive:false});
    canvas.addEventListener('dblclick',goLive);
    canvas.tabIndex=0;canvas.title='Drag to pan; scroll or pinch to zoom; double-click for live';
    canvas.addEventListener('keydown',e=>{const range=windowRange();if(e.key==='ArrowLeft'||e.key==='ArrowRight')setView(range.end+(e.key==='ArrowLeft'?-.2:.2)*range.span,range.span);else if(e.key==='+'||e.key==='=')zoomAt(.5,.5);else if(e.key==='-')zoomAt(2,.5);else if(e.key==='Home')goLive();else return;e.preventDefault();});
  }
  function select(id){store.selected=id;store.autoSelect=false;detailSignature='';store.changed=true;tooltip.hidden=true;connectionPackets.update(store);}
  function showTip(event,text){tooltip.textContent=text;tooltip.hidden=false;tooltip.style.left=Math.max(8,Math.min(event.clientX+12,innerWidth-340))+'px';tooltip.style.top=Math.max(8,Math.min(event.clientY+14,innerHeight-tooltip.offsetHeight-12))+'px';}
  const packetTip = p => `CH ${p.channel} · ${frequency(p.channel)} MHz · ${seconds(p.capture_s)}\n${p.crc_valid===false?'CRC FAILED · unverified bytes':p.kind || 'Data packet'} · ${p.pdu.length/2} bytes\nAA ${p.access_address}${p.connection_event!=null?'\nInferred event '+p.connection_event:''}${packetName(p)?'\n'+packetName(p):''}${Number.isFinite(p.signal_dbfs)?'\nSignal '+p.signal_dbfs.toFixed(1)+' dBFS':''}${Number.isFinite(p.noise_dbfs)?'\nNoise estimate '+p.noise_dbfs.toFixed(1)+' dBFS':''}`;
  function matches(p){return visiblePacket(p)&&(store.filterChannel===null||p.channel===store.filterChannel)&&(!store.filterKind||(store.filterKind==='ads')===(p.access_address===ADV))&&(!store.filterQuery||(JSON.stringify(p)+' '+packetName(p)).toLowerCase().includes(store.filterQuery));}
  function refilter(){store.filtered=store.packets.filter(matches);$('packet-scroll').scrollTop=0;renderList();}
  function receive(data){
    if(store.session!==data.session){
      devices=new PacketModel.DeviceRegistry();deviceRenderAt=-Infinity;store.session=data.session;autoLevels=new PlotMath.AutoLevels();lastGainUpdate=-Infinity;lastGainEnd=null;store.packets=[];store.spectrum=[];store.connections.clear();store.byConnection.clear();store.filtered=[];
      store.cursor=store.spectrumCursor=0;store.selected=null;store.autoSelect=true;store.frozen=null;previousRate=null;
      $('timeline-back').value=0;$('timeline-freeze').textContent='Ⅱ Pause';$('timeline-freeze').setAttribute('aria-pressed','false');
      store.listFrozen=[];detailSignature='';rowNodes.clear();$('connection-rows').replaceChildren();
    }
    const incoming=data.packets.filter(p=>p.id>store.cursor);
    const oldTop=$('packet-scroll').scrollTop;
    const namesBefore=devices.namesRevision;
    let matched=0;
    for(const p of incoming){
      const device=devices.ingest(p);if(device){p.device_key=device.key;p.address=device.address;}
      store.packets.push(p);
      if(matches(p)){store.filtered.push(p);matched++;}
      if(p.connection_id&&p.access_address!==ADV){
        if(!store.byConnection.has(p.connection_id))store.byConnection.set(p.connection_id,[]);
        store.byConnection.get(p.connection_id).push(p);
      }
    }
    if(store.packets.length>LIMIT){
      store.packets.splice(0,store.packets.length-LIMIT);
      const first=store.packets[0].id;
      store.filtered=store.filtered.filter(p=>p.id>=first);
      for(const [id,points] of store.byConnection){let cut=0;while(cut<points.length&&points[cut].id<first)cut++;if(cut)points.splice(0,cut);}
    }
    if(store.filterQuery&&devices.namesRevision!==namesBefore)store.filtered=store.packets.filter(matches);
    if(oldTop>1&&matched&&!store.pausedList&&$('packet-log').open)$('packet-scroll').scrollTop=oldTop+matched*ROW_HEIGHT;
    for(const line of data.spectrum||[]) if(line.id>store.spectrumCursor)store.spectrum.push(line);
    if(store.spectrum.length>6000)store.spectrum.splice(0,store.spectrum.length-6000);
    const keep=new Set();
    for(const c of data.connections||[]){
      keep.add(c.id);
      const old=store.connections.get(c.id), markers=old?old.markers:[];
      const last=markers.length?markers[markers.length-1].id:0;
      c.markers=markers.concat((c.markers||[]).filter(p=>p.id>last)).slice(-128);
      store.connections.set(c.id,c);
    }
    for(const id of store.connections.keys())if(!keep.has(id)){store.connections.delete(id);store.byConnection.delete(id);}
    if(store.selected&&!keep.has(store.selected))store.selected=null;
    if(store.autoSelect&&store.connections.size)store.selected=[...store.connections.values()].sort((a,b)=>Number(b.observed)-Number(a.observed)||b.count-a.count||a.start-b.start)[0].id;
    store.cursor=data.cursor;store.spectrumCursor=data.spectrum_cursor||0;store.status=data.status;store.gaps=data.gaps||[];store.receiver=data.receiver||{};
    store.active=data.active;store.connected=true;store.lastMessage=performance.now();
    store.clock=Math.max(data.capture_s||0,store.spectrum.length?store.spectrum[store.spectrum.length-1].end_s:0);
    store.receivedAt=performance.now();store.revision++;
    if(data.history_expired){$('notice').hidden=false;setText('notice','The browser fell behind the retained history. Earlier packets expired; current reception continues.');}
    const stat=store.status;
    if(previousRate&&stat.elapsed_s>previousRate.time){packetRate=(stat.packets-previousRate.count)/(stat.elapsed_s-previousRate.time);}
    if(!previousRate||stat.elapsed_s>previousRate.time)previousRate={count:stat.packets||0,time:stat.elapsed_s||0};
    store.changed=true;
  }
  const feed=new EventSource('/events');
  feed.onmessage=e=>{try{receive(JSON.parse(e.data));}catch(error){console.error('Viewer update failed',error);}};
  feed.onerror=()=>{store.connected=false;store.changed=true;};
  function updateUI(){
    syncNavigation();
    setText('timeline-mode',store.packets.length>=LIMIT?'Latest 50,000 packets':'Individual packets');
    const active=store.active&&store.connected&&performance.now()-store.lastMessage<2000;
    const receiver=store.receiver||{};
    const starting=['detecting','starting','authorizing'].includes(receiver.phase);
    $('live').className='status'+(active?'':' off');$('live').replaceChildren(node('i'),document.createTextNode(active?'Receiving':receiver.phase==='authorizing'?'Approval needed':starting?'Starting':store.connected?'Stream stopped':'Reconnecting'));
    setText('receiver-mode',receiver.message||'Checking receiver');
    setText('processing',store.status.backend==='metal'?'GPU channelizer · Metal':'CPU channelizer');
    setText('packets',number(store.status.packets));setText('packet-rate',number(Math.max(0,Math.round(packetRate)))+' packets / sec');
    setText('connection-total',store.connections.size);
    const setups=[...store.connections.values()].filter(c=>c.observed).length;
    setText('setup-total',setups+' setups · '+(store.connections.size-setups)+' joined midstream');setText('connection-count',store.connections.size);
    const channels=store.status.channels||[];const seen=channels.filter(c=>c.packets>0).length;
    $('seen').replaceChildren(document.createTextNode(seen+' '),node('em','/ 40'));
    const missing=(store.status.lost_samples||0)/160000;
    setText('health',active?missing?'Gaps detected':'Receiving':starting?'Starting':store.connected?'Stopped':'Connecting');
    $('health').style.color=active&&!missing?'#2b8e75':'#a48b65';
    setText('lag',(store.status.lag_ms||0).toFixed(1)+' ms');setText('lost',missing.toFixed(1)+' ms');
    setText('capture-clock',seconds(clockNow())+' / capture');
    for(const c of channels){const b=$('ch'+c.channel);b.classList.toggle('has-packets',c.packets>0);b.title=`Channel ${c.channel} · ${c.mhz} MHz · ${number(c.packets)} packets`;}
    syncRows();inspector();connectionPackets.update(store);renderList();renderDevices();
  }
  function syncRows(){
    let rows=[...store.connections.values()];const mode=$('timeline-filter').value;
    if(mode==='observed')rows=rows.filter(c=>c.observed);
    if(mode==='active')rows=rows.filter(c=>!c.ended&&!c.quiet);
    rows.sort((a,b)=>Number(b.observed)-Number(a.observed)||b.count-a.count||a.start-b.start);
    const list=$('connection-rows'), ids=new Set(rows.map(c=>c.id));
    for(const [id,r] of rowNodes)if(!ids.has(id)){r.row.remove();rowNodes.delete(id);}
    const empty=list.querySelector('.timeline-empty');
    if(!rows.length){if(!empty){const box=node('div',null,'timeline-empty');box.append(node('div','╎ ╎ ╎ ╎ ╎','empty-signal'),node('b',mode==='observed'?'Waiting for a connection setup':'Listening for connections'),node('span',mode==='observed'?'New CONNECT_IND packets will appear here. Existing connections are available under “All connections”.':'Each connection gets a lane. Packet marks will follow its activity across the band.'));list.append(box);}return;}
    if(empty)empty.remove();
    rows.forEach((c,index)=>{
      let r=rowNodes.get(c.id);
      if(!r){
        const row=node('div',null,'connection-row'),label=node('button',null,'connection-label'),title=node('b'),sub=node('small'),canvas=node('canvas',null,'connection-track');
        label.append(title,sub);label.onclick=()=>select(c.id);canvas.setAttribute('role','img');canvas.onclick=()=>select(c.id);
        canvas.onpointermove=e=>{const rect=canvas.getBoundingClientRect(),px=e.clientX-rect.left;let best=null,dist=10;
          for(const p of r.points||[]){const d=Math.abs(p.x-px);if(d<dist){best=p;dist=d;}}
          if(!best){tooltip.hidden=true;return;}showTip(e,best.packet?packetTip(best.packet):`${seconds(best.time)}\n${best.label}`);};
        canvas.onpointerleave=()=>tooltip.hidden=true;
        navigatePlot(canvas);
        row.append(label,canvas);r={row,label,title,sub,canvas,points:[]};rowNodes.set(c.id,r);
      }
      if(list.children[index]!==r.row)list.insertBefore(r.row,list.children[index]||null);
      r.row.classList.toggle('selected',store.selected===c.id);r.label.classList.toggle('selected',store.selected===c.id);r.label.setAttribute('aria-pressed',String(store.selected===c.id));
      r.title.textContent=connectionName(c)||c.access_address;
      const dot=node('i',null,'row-status'+(c.quiet||c.ended?' quiet':''));
      r.sub.replaceChildren(dot,document.createTextNode((connectionName(c)?c.access_address:c.observed?'Setup captured':'Joined midstream')+' · '+number(c.count)));
      r.label.title=c.observed?`${deviceLabel(c.central)} → ${deviceLabel(c.peripheral)} · AA ${c.access_address}`:`${c.access_address} · ${number(c.count)} packets · setup not captured`;
      r.canvas.setAttribute('aria-label',`Connection ${c.access_address}, ${c.count} captured packets`);
    });
  }
  function inspector(){
    const c=store.connections.get(store.selected), signature=c?JSON.stringify([c.id,c.count,c.used,c.warning,c.state,c.interval_ms,c.markers.length,deviceLabel(c.central),deviceLabel(c.peripheral)]):'empty';
    if(signature===detailSignature)return;detailSignature=signature;
    const box=$('connection-detail');box.replaceChildren();$('clear-selection').disabled=!c;
    if(!c){const empty=node('div',null,'inspector-empty');empty.append(node('div','⌖','empty-symbol'),node('b','A closer look'),node('p','Select a connection in the timeline or click one of its packets in the waterfall.'));box.append(empty);return;}
    const top=node('div',null,'detail-top'),bottom=node('div',null,'detail-bottom');
    top.append(node('span','ACCESS ADDRESS','inspector-eyebrow'),node('div',c.access_address,'connection-id'),node('span',c.observed?'Setup captured':'Joined midstream','badge'+(c.observed?' observed':'')));
    if(c.observed){const pair=node('div',null,'device-pair');for(const [label,value] of [['Central',c.central],['Peripheral',c.peripheral]]){const item=node('div');item.append(node('span',label),node('b',deviceLabel(value)));pair.append(item);}top.append(pair);}
    const stats=node('div',null,'detail-stats');
    for(const [value,label] of [[number(c.count),'Packets'],[c.channels.length,'Channels'],[c.interval_ms==null?'—':c.interval_ms+' ms','Interval'],[c.map_updates,'Map updates']]){const item=node('div');item.append(node('strong',value),node('small',label));stats.append(item);}top.append(stats);
    const facts=node('dl',null,'facts');for(const [k,v] of [['CRC seed',c.crc_init],['Channel selection',c.algorithms.length===1?'CSA #'+c.algorithms[0]:c.observed?'Unresolved':'Unknown'],['State',c.ended?c.state:c.quiet?'Quiet':c.encrypted?'Encryption seen':'Traffic observed']]){const row=node('div');row.append(node('dt',k),node('dd',v));facts.append(row);}top.append(facts);
    if(c.warning)top.append(node('p',c.warning,'detail-note'));
    if(c.used.length){const heading=node('div',null,'detail-section-title');heading.append(node('span','Channel map'),node('small',c.used.length+' / 37 used'));bottom.append(heading);const map=node('div',null,'channel-map');for(let ch=0;ch<37;ch++){const tile=node('span',ch,c.used.includes(ch)?'used':'');tile.title=`Channel ${ch}: ${c.used.includes(ch)?'enabled':'excluded'}`;map.append(tile);}bottom.append(map);}
    for(const m of c.pending_maps)bottom.append(node('p',`${m.used.length}-channel map pending at event ${m.event&65535}.`,'detail-note'));
    if(c.markers.length){bottom.append(node('div','Connection events','detail-section-title'));const log=node('ol',null,'event-log');for(const p of c.markers.slice(-8).reverse()){const li=node('li',p.label||p.kind);li.append(node('time',seconds(p.time)));log.append(li);}bottom.append(log);}
    if(!c.observed)bottom.append(node('p','Device addresses and the hopping map become available when a connection setup is captured.','chart-note'));
    box.append(top,bottom);
  }
  function packetName(p){return devices.devices.get(p.device_key)?.name||p.name||'';}
  function deviceLabel(address){const name=devices.lookup(address)?.name;return name?name+' · '+address:address;}
  function connectionName(c){return devices.lookup(c.peripheral)?.name||devices.lookup(c.central)?.name||'';}
  function renderDevices(force=false){
    const now=performance.now();if(!force&&now-deviceRenderAt<1000)return;deviceRenderAt=now;
    const all=[...devices.devices.values()],query=$('device-search').value.toLowerCase();
    const rows=all.filter(d=>(d.name+' '+d.address).toLowerCase().includes(query)).sort((a,b)=>Number(!!b.name)-Number(!!a.name)||(a.name||a.address).localeCompare(b.name||b.address));
    setText('device-count',all.filter(d=>d.name).length+' named / '+all.length+' addresses');
    const fragment=document.createDocumentFragment();
    for(const d of rows.slice(0,300)){
      const row=node('div',null,'device-row'),identity=node('div');
      identity.append(node('b',d.name||'Unnamed device'),node('small',d.address+' · '+(d.random?'random':'public')));
      row.append(identity,node('span',d.name?(d.complete?'Complete':'Shortened')+' · '+d.source:'No name captured'),node('span',Number.isFinite(d.signal)?d.signal.toFixed(1):'—','mono'),node('span',Math.max(0,store.clock-d.last).toFixed(1)+' s ago','mono'));
      fragment.append(row);
    }
    if(!rows.length)fragment.append(node('p',query?'No matching devices.':'Listening for advertisements…','chart-note'));
    $('device-rows').replaceChildren(fragment);
    setText('device-retention',`Showing ${Math.min(300,rows.length)} of ${rows.length} matching addresses · Names retained while this capture is open (up to 10,000 addresses).`);
  }
  $('device-search').oninput=()=>renderDevices(true);
  function surface(canvas,height){const w=Math.max(1,Math.round(canvas.clientWidth)),h=height,ratio=Math.min(2,devicePixelRatio||1);if(canvas.width!==Math.round(w*ratio)||canvas.height!==Math.round(h*ratio)){canvas.width=w*ratio;canvas.height=h*ratio;}const ctx=canvas.getContext('2d');ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,w,h);return {ctx,w,h};}
  function drawTimeline(range){
    const {start,end,span}=range;setText('timeline-range',seconds(start)+' – '+seconds(end));
    const ticks=$('timeline-ticks');if(!ticks.children.length)for(let i=0;i<5;i++)ticks.append(node('span'));
    for(let i=0;i<5;i++)ticks.children[i].textContent=(start+span*i/4).toFixed(span<.01?6:span<1?3:1)+' s';
    for(const [id,r] of rowNodes){
      const c=store.connections.get(id);if(!c)continue;
      const {ctx,w,h}=surface(r.canvas,64),x=t=>(t-start)/span*w;r.points=[];
      ctx.strokeStyle='#243847';ctx.lineWidth=.5;for(let i=0;i<=4;i++){ctx.beginPath();ctx.moveTo(i*w/4,0);ctx.lineTo(i*w/4,h);ctx.stroke();}
      ctx.strokeStyle='#274251';ctx.setLineDash([2,4]);ctx.beginPath();ctx.moveTo(0,36);ctx.lineTo(w,36);ctx.stroke();ctx.setLineDash([]);
      for(const g of store.gaps)if(g.end>=start&&g.start<=end){ctx.fillStyle='#a5585e35';ctx.fillRect(Math.max(0,x(g.start)),0,Math.max(2,x(Math.min(end,g.end))-Math.max(0,x(g.start))),h);}
      const points=store.byConnection.get(id)||[];
      for(let i=points.length-1;i>=0;i--){const p=points[i];if(p.capture_s<start-.003)break;if(p.capture_s<start||p.capture_s>end||!visiblePacket(p))continue;
        const px=x(p.capture_s);ctx.fillStyle=p.crc_valid===false?'#ff8585':channelColor(p.channel);ctx.globalAlpha=store.selected===id?1:.75;
        ctx.fillRect(px,27,Math.max(1.2,Math.min(12,(p.pdu.length/2+8)*8e-6/span*w)),18);
        if(span<=1){ctx.font='8px ui-monospace,monospace';ctx.fillText(String(p.channel),px+2,58);}
        r.points.push({x:px,packet:p});
      }
      ctx.globalAlpha=1;
      for(const p of c.markers)if(p.time>=start&&p.time<=end){const px=x(p.time);ctx.fillStyle=p.kind==='connect'?'#bdeaf2':p.kind==='terminate'?'#e59a9c':'#eac17b';ctx.save();ctx.translate(px,14);ctx.rotate(Math.PI/4);ctx.fillRect(-3,-3,6,6);ctx.restore();r.points.push({x:px,...p});}
    }
  }
  function waterfallLevels(end) {
    const time=performance.now(),automatic=$('waterfall-auto').checked;
    if(automatic&&time-lastGainUpdate>=200&&(store.frozen===null||end!==lastGainEnd)) {
      // Freeze the scale along with a paused plot. Refit when navigating history.
      if(store.frozen!==null&&end!==lastGainEnd)autoLevels.initialized=false;
      autoLevels.update(store.spectrum,end,Number.isFinite(lastGainUpdate)?(time-lastGainUpdate)/1000:0);
      lastGainUpdate=time;lastGainEnd=end;
      $('waterfall-floor').value=autoLevels.low;$('waterfall-ceiling').value=autoLevels.high;
    }
    const low=automatic?autoLevels.low:Number($('waterfall-floor').value),high=automatic?autoLevels.high:Number($('waterfall-ceiling').value);
    setText('floor-value',low.toFixed(1)+' dBFS');setText('ceiling-value',high.toFixed(1)+' dBFS');
    return {low,high};
  }
  function drawWaterfall({start,end,span}){
    const {ctx,w,h}=surface(water,Math.round(water.clientHeight)),lines=store.spectrum;
    $('waterfall-empty').hidden=lines.length>0;setText('waterfall-duration',span<1?(span*1000).toFixed(0)+' ms':span.toFixed(0)+' s');
    if(!imageData||imageData.height!==h){raster.width=40;raster.height=h;imageData=rasterContext.createImageData(40,h);}
    const pixels=imageData.data,{low,high}=waterfallLevels(end);
    const averaged=PlotMath.spectrumRows(lines,start,end,h);
    for(let y=0;y<h;y++)for(let x=0;x<40;x++){
      const off=(y*40+x)*4,db=averaged[y*40+order[x]],color=Number.isFinite(db)?lut[Math.max(0,Math.min(255,Math.round((db-low)/(high-low)*255)))]:[10,22,34];
      pixels[off]=color[0];pixels[off+1]=color[1];pixels[off+2]=color[2];pixels[off+3]=255;
    }
    rasterContext.putImageData(imageData,0,0);ctx.imageSmoothingEnabled=false;ctx.drawImage(raster,0,0,w,h);
    ctx.strokeStyle='#ffffff0c';ctx.lineWidth=.5;for(let i=1;i<40;i++){ctx.beginPath();ctx.moveTo(i*w/40,0);ctx.lineTo(i*w/40,h);ctx.stroke();}
    for(const g of store.gaps)if(g.end>=start&&g.start<=end){ctx.fillStyle='#d67f7f30';const a=Math.max(0,(end-g.end)/span*h),b=Math.min(h,(end-g.start)/span*h);ctx.fillRect(0,a,w,Math.max(2,b-a));}
    pointerPoints=[];
    if($('waterfall-packets').checked){
      for(let i=store.packets.length-1;i>=0;i--){const p=store.packets[i],t=p.capture_s;if(t<start-.003)break;if(t<start||t>end||!visiblePacket(p))continue;
        const x=column.get(p.channel)*w/40+1,y=(end-t)/span*h,selected=p.connection_id&&p.connection_id===store.selected;
        const height=Math.max(2.5,Math.min(14,(p.pdu.length/2+8)*8e-6/span*h));
        ctx.strokeStyle=p.crc_valid===false?'#ff8585':selected?'#9affdd':p.access_address===ADV?'#f5cb80':'#8ecbfa';ctx.globalAlpha=selected?1:.72;ctx.lineWidth=selected?1.5:1;
        ctx.strokeRect(x,Math.min(h-height,y),Math.max(2,w/40-2),height);pointerPoints.push({x,y,width:w/40,height,packet:p});
      }
      ctx.globalAlpha=1;
    }
    ctx.font='9px ui-monospace,monospace';ctx.textAlign='right';for(let k=1;k<4;k++){const y=k*h/4;ctx.fillStyle='#0c1d29bd';ctx.fillRect(w-42,y-9,42,13);ctx.fillStyle='#b4c5d0';ctx.fillText('−'+(span*k/4).toFixed(span<1?2:1)+'s',w-4,y);}
    ctx.textAlign='left';
  }
  function hitWater(event){const r=water.getBoundingClientRect(),x=event.clientX-r.left,y=event.clientY-r.top;let best=null,distance=7;for(const point of pointerPoints){const d=Math.abs(point.y-y);if(x>=point.x-2&&x<=point.x+point.width&&d<distance){best=point;distance=d;}}return best;}
  water.onpointermove=e=>{const point=hitWater(e);if(point)showTip(e,packetTip(point.packet));else tooltip.hidden=true;};
  water.onpointerleave=()=>tooltip.hidden=true;
  water.onclick=e=>{const point=hitWater(e);if(point&&point.packet.connection_id)select(point.packet.connection_id);};
  navigatePlot(water,true);
  navigatePlot($('timeline-ticks'));
  function renderList(){
    if(!$('packet-log').open)return;
    const source=store.pausedList?store.listFrozen.filter(matches):store.filtered;
    const scroll=$('packet-scroll'),start=Math.max(0,Math.floor(scroll.scrollTop/ROW_HEIGHT)-3),count=Math.ceil(scroll.clientHeight/ROW_HEIGHT)+6;
    $('packet-spacer').style.height=source.length*ROW_HEIGHT+'px';$('packet-rows').style.transform=`translateY(${start*ROW_HEIGHT}px)`;
    const frag=document.createDocumentFragment();
    for(let i=start;i<Math.min(source.length,start+count);i++){
      const p=source[source.length-1-i],row=node('div',null,'packet-row'+(p.crc_valid===false?' crc-failed':''));
      for(const [value,css] of [[seconds(p.capture_s),'mono'],['CH '+p.channel,'mono'],[p.kind+(packetName(p)?' · '+packetName(p):p.address?' · '+p.address:''),''],[p.access_address+' · '+p.pdu,'mono']])row.append(node('span',value,css));
      row.title=`#${p.id} · ${p.pdu.length/2} bytes · CRC ${p.crc} · ${p.pdu}`;row.onclick=()=>{if(p.connection_id)select(p.connection_id);};frag.append(row);
    }
    $('packet-rows').replaceChildren(frag);setText('packet-matches',number(source.length)+' matching packets');
    setText('packet-retention',store.packets.length?`Retained #${store.packets[0].id}–${store.cursor}`:'');
  }
  for(const ch of order){const b=node('button',ch);b.id='ch'+ch;b.setAttribute('aria-label','Filter channel '+ch);b.onclick=()=>{store.filterChannel=store.filterChannel===ch?null:ch;syncChannelFilter();refilter();};$('channels').append(b);}
  function syncChannelFilter(){for(const ch of order){$('ch'+ch).classList.toggle('active',store.filterChannel===ch);$('ch'+ch).setAttribute('aria-pressed',String(store.filterChannel===ch));}setText('all',store.filterChannel===null?'All channels':'CH '+store.filterChannel+' · Clear');}
  $('search').oninput=()=>{store.filterQuery=$('search').value.toLowerCase();refilter();};$('kind').onchange=()=>{store.filterKind=$('kind').value;refilter();};$('all').onclick=()=>{store.filterChannel=null;syncChannelFilter();refilter();};
  $('pause').onclick=()=>{store.pausedList=!store.pausedList;store.listFrozen=store.pausedList?store.packets.slice():[];setText('pause',store.pausedList?'Resume list':'Pause list');renderList();};
  $('packet-scroll').onscroll=renderList;$('packet-log').ontoggle=renderList;
  $('show-crc-failures').onchange=()=>{
    store.showCrcFailures=$('show-crc-failures').checked;
    try {localStorage.setItem('airspace-show-crc-failures',String(store.showCrcFailures));} catch {}
    store.changed=true;store.revision++;refilter();
    connectionPackets.revision=-1;connectionPackets.update(store);
  };
  $('timeline-filter').onchange=()=>{syncRows();store.changed=true;};
  $('timeline-span').onchange=()=>{const span=Number($('timeline-span').value);if(!Number.isFinite(span))return;store.span=span;if(store.frozen!==null)setView(store.frozen,span);else syncNavigation();};
  $('timeline-freeze').onclick=()=>{if(store.frozen===null)setView(clockNow(),store.span);else goLive();};
  $('timeline-back').oninput=()=>setView(store.clock-Number($('timeline-back').value),store.span);
  $('timeline-latest').onclick=goLive;
  $('clear-selection').onclick=()=>select(null);
  $('waterfall-packets').onchange=()=>store.changed=true;
  $('waterfall-auto').onchange=()=>{
    const automatic=$('waterfall-auto').checked;
    $('waterfall-floor').disabled=$('waterfall-ceiling').disabled=automatic;
    autoLevels.initialized=false;lastGainUpdate=-Infinity;lastGainEnd=null;store.changed=true;
  };
  for(const id of ['waterfall-floor','waterfall-ceiling'])$(id).oninput=()=>{
    const low=Number($('waterfall-floor').value),high=Number($('waterfall-ceiling').value);
    if(high-low<5){if(id==='waterfall-floor')$('waterfall-ceiling').value=low+5;else $('waterfall-floor').value=high-5;}
    store.changed=true;
  };
  new ResizeObserver(()=>store.changed=true).observe(document.body);
  function frame(time){
    if(!document.hidden){
      if(time-lastUI>=200){updateUI();lastUI=time;}
      if(time-lastPaint>=33&&(store.changed||(store.active&&store.frozen===null))){const range=windowRange();drawTimeline(range);drawWaterfall(range);store.changed=false;lastPaint=time;}
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
})();
