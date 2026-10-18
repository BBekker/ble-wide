// SPDX-License-Identifier: MIT
const assert=require('node:assert/strict');
const {view,zoom,spectrumRows}=require('../../web/plot-math.js');
const near=(a,b)=>assert.ok(Math.abs(a-b)<1e-8,`${a} != ${b}`);
// Zoom stays under the pointer and panning clamps to retained capture history.
let r={start:60,end:90,span:30};
for(const anchor of [0,.25,.5,1]){
  const z=zoom(r,.2,anchor,0,100);
  near(z.start+anchor*z.span,r.start+anchor*r.span);
}
assert.deepEqual(view(500,30,20,100),{start:70,end:100,span:30});
assert.deepEqual(view(-50,30,20,100),{start:20,end:50,span:30});
near(zoom(r,1e-9,.5,0,100).span,.001);
near(zoom(r,1e9,.5,0,100).span,120);
// Average in linear power, including every source row rather than one sample.
const line=(a,b,db)=>({capture_s:a,end_s:b,db:Array(40).fill(db)});
let rows=[line(0,.02,-20),line(.02,.04,0),line(.04,.06,-20)];
let result=spectrumRows(rows,0,.06,1);
near(result[0],10*Math.log10(1.02/3));
result=spectrumRows(rows,.01,.05,1);
near(result[39],10*Math.log10(.505));
// A short burst survives at wide zoom; uncovered intervals stay blank.
rows=Array.from({length:100},(_,i)=>line(i*.02,(i+1)*.02,i===50?0:-100));
result=spectrumRows(rows,0,2,10);
assert.ok(Math.max(...result)>-11);
result=spectrumRows([line(0,.02,-30),line(.04,.06,-40)],0,.06,3);
near(result[0],-40);assert.ok(Number.isNaN(result[40]));near(result[80],-30);
// Sub-row zoom retains constant power without flickering at row boundaries.
for(let t=.002;t<.05;t+=.001){result=spectrumRows([line(0,.02,-30),line(.02,.04,-30),line(.04,.06,-30)],t,t+.005,50);for(const v of result)near(v,-30);}
console.log('Plot navigation and waterfall integration tests passed');
// Automatic display levels use robust signal statistics and move both endpoints.
const {AutoLevels}=require('../../web/plot-math.js');
const powers=(offset=0)=>Array.from({length:100},(_,i)=>({capture_s:i*.02,end_s:(i+1)*.02,db:Array.from({length:40},(_,ch)=>offset+(ch<36?-60:-30))}));
let gain=new AutoLevels();assert.equal(gain.update(powers(),2,.2),true);
near(gain.low,-63);near(gain.high,-27);
const oldLow=gain.low,oldHigh=gain.high;
gain.update(powers(20),2,.2);
assert.ok(gain.low>oldLow&&gain.low<oldLow+20);
assert.ok(gain.high>oldHigh&&gain.high<oldHigh+20);
for(let i=0;i<150;i++)gain.update(powers(20),2,.2);
assert.ok(Math.abs(gain.low+43)<.01);assert.ok(Math.abs(gain.high+7)<.01);
// A single extreme sample must not wash out the whole waterfall.
let robust=powers();robust[50].db[0]=20;
let outlier=new AutoLevels();outlier.update(robust,2,.2);near(outlier.high,-27);
let quiet=new AutoLevels();quiet.update([line(0,1,-70)],1,.2);assert.ok(quiet.high-quiet.low>=18);
let frozen=[quiet.low,quiet.high];assert.equal(quiet.update([],1,.2),false);assert.deepEqual([quiet.low,quiet.high],frozen);
assert.equal(quiet.update([line(0,1,-120)],1,.2),false);assert.deepEqual([quiet.low,quiet.high],frozen);
// History inspection ignores future rows and stale values outside its two seconds.
let history=new AutoLevels();history.update([...powers(),line(3,4,0)],2,.2);near(history.high,-27);
assert.equal(history.update(powers(),10,.2),false);
console.log('Automatic waterfall level tests passed');
