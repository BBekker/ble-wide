// SPDX-License-Identifier: MIT
// Pure time-window and power-resampling math, shared with the regression tests.
(function(root) {
  const clamp = (x,a,b) => Math.max(a,Math.min(b,x));
  function view(end,span,earliest,latest) {
    span=clamp(span,.001,120);
    end=clamp(end,Math.min(latest,earliest+span),latest);
    return {start:Math.max(0,end-span),end,span};
  }
  function zoom(range,factor,anchor,earliest,latest) {
    anchor=clamp(anchor,0,1);
    const span=clamp(range.span*factor,.001,120);
    return view(range.start+anchor*range.span+(1-anchor)*span,span,earliest,latest);
  }
  function spectrumRows(lines,start,end,height) {
    // Integrate linear power over each screen row's time interval. Point
    // sampling aliases bursts as the display scrolls past the 20 ms FFT rows.
    const step=(end-start)/height, output=new Float64Array(height*40).fill(NaN);
    if(step<=0)return output;
    let index=lines.length-1;
    for(let y=0;y<height;y++) {
      const hi=end-y*step,lo=hi-step;
      while(index>=0&&lines[index].capture_s>=hi)index--;
      const sums=new Float64Array(40);let duration=0;
      for(let k=index;k>=0&&lines[k].end_s>lo;k--) {
        const line=lines[k],weight=Math.max(0,Math.min(hi,line.end_s)-Math.max(lo,line.capture_s));
        if(weight<1e-10)continue;
        const powers=line.power||(line.power=Float64Array.from(line.db,v=>10**(v/10)));
        for(let ch=0;ch<40;ch++)sums[ch]+=powers[ch]*weight;
        duration+=weight;
      }
      // Uncovered time remains empty; input-gap overlays show partial gaps.
      if(duration>0)for(let ch=0;ch<40;ch++)output[y*40+ch]=10*Math.log10(Math.max(1e-12,sums[ch]/duration));
    }
    return output;
  }
  class AutoLevels {
    constructor() {this.low=-65;this.high=-15;this.initialized=false;}
    update(lines,end,dt) {
      // Robust limits from two seconds of actual FFT bins, never packet outlines.
      // A histogram avoids sorting thousands of powers at every display update.
      const bins=new Uint32Array(141);let count=0;
      for(let i=lines.length-1;i>=0;i--) {
        const line=lines[i];if(line.end_s<end-2)break;if(line.capture_s>end)continue;
        for(const db of line.db)if(Number.isFinite(db)&&db>-119.9) {
          bins[clamp(Math.round(db)+120,0,140)]++;count++;
        }
      }
      if(!count)return false; // Missing input and digital silence cannot set gain.
      const percentile=q=>{const rank=Math.max(1,Math.ceil(count*q));let total=0;for(let i=0;i<bins.length;i++){total+=bins[i];if(total>=rank)return i-120;}return 20;};
      const low=clamp(percentile(.05)-3,-120,2);
      const high=clamp(Math.max(low+18,percentile(.99)+3),low+18,20);
      if(!this.initialized){this.low=low;this.high=high;this.initialized=true;}
      else {
        dt=clamp(dt,0,1);
        this.low+=(low-this.low)*(1-Math.exp(-dt/3));
        // Make room for stronger signals quickly, release slowly to avoid pumping.
        this.high+=(high-this.high)*(1-Math.exp(-dt/(high>this.high?.5:4)));
        this.high=Math.max(this.high,this.low+18);
      }
      return true;
    }
  }
  const api={view,zoom,spectrumRows,AutoLevels};
  if(typeof module==='object'&&module.exports)module.exports=api;else root.PlotMath=api;
})(typeof globalThis==='object'?globalThis:this);
