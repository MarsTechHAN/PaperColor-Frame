// evalharness.mjs — computational quality scorer for the E6 color pipeline.
//
// The methodology fix: stop eyeball-flashing. This loads the real dither.wasm,
// renders a fixed test set, predicts the on-panel appearance with the FAITHFUL
// Yule–Nielsen forward model (the one validated to ~1 ΔE vs the 28 measured
// patches), and reports a scorecard whose terms map 1:1 to the user's
// complaints:
//   fidelity   — mean ΔE2000(predicted panel, source)         (lower = truer to phone)
//   skinΔE     — mean ΔE2000 on skin swatches                  (faces)
//   chromaRet  — predicted C / source C on saturated swatches  (vividness; want high, ≤~1)
//   overSat    — predicted C minus source C where predicted>source (太过)
//   speckle    — colored-ink fraction on low-chroma swatches   (麻点)
//   drSpread   — predicted L p-range over the neutral ramp      (dynamic-range use)
//
// Usage:  bash web_wasm/build.sh && node tools/wfsim/evalharness.mjs [mode]
//   mode = render-mode int (default 2 = reflectance v2)
import { readFileSync } from 'node:fs';

const MODE = parseInt(process.argv[2] ?? '2', 10);
const PW = 400, PH = 600, SW = 600, SH = 900;
const WASM = new URL('../../main/web/dither.wasm', import.meta.url);

// ---- measured panel ink Lab (SCI) in PIDX order K,W,Y,R,B,G — the real panel appearance
const INK = [
  [30.62, 2.96, -6.39], [65.47, -3.54, -1.23], [62.87, -7.51, 45.08],
  [35.65, 21.56, 8.63], [40.67, -2.34, -24.71], [41.05, -14.58, 11.83],
];
const YN_N = 1.4;

// ---- color math
const cbrt = Math.cbrt;
function labToXyz(L, a, b) {
  const fy = (L + 16) / 116, fx = a / 500 + fy, fz = fy - b / 200;
  const fi = t => { const t3 = t*t*t; return t3 > 0.008856 ? t3 : (t - 16/116) / 7.787; };
  return [0.95047 * fi(fx), fi(fy), 1.08883 * fi(fz)];
}
function xyzToLab(X, Y, Z) {
  const f = t => t > 0.008856 ? cbrt(t) : 7.787*t + 16/116;
  const fx = f(X/0.95047), fy = f(Y), fz = f(Z/1.08883);
  return [116*fy - 16, 500*(fx - fy), 200*(fy - fz)];
}
const _lin = new Float32Array(256);
for (let i = 0; i < 256; i++) { const c = i/255; _lin[i] = c <= 0.04045 ? c/12.92 : Math.pow((c+0.055)/1.055, 2.4); }
function srgbToLab(r, g, b) {
  const R=_lin[r|0], G=_lin[g|0], B=_lin[b|0];
  return xyzToLab((0.4124*R+0.3576*G+0.1805*B)/0.95047*0.95047, 0.2126*R+0.7152*G+0.0722*B, (0.0193*R+0.1192*G+0.9505*B)/1.08883*1.08883);
}
// precompute ink pseudo-reflectance p = XYZ^(1/n)
const INK_P = INK.map(([L,a,b]) => labToXyz(L,a,b).map(v => Math.pow(Math.max(0,v), 1/YN_N)));
function ynPredict(frac) {           // frac[6] area fractions → predicted Lab
  let tot = 0; for (const f of frac) tot += f; if (tot <= 0) return [50,0,0];
  const acc = [0,0,0];
  for (let i = 0; i < 6; i++) { const f = frac[i]/tot; if (f<=0) continue; for (let c=0;c<3;c++) acc[c] += f*INK_P[i][c]; }
  return xyzToLab(...acc.map(v => Math.pow(Math.max(0,v), YN_N)));
}
function ciede2000(L1,a1,b1,L2,a2,b2){
  const rad=Math.PI/180, C1=Math.hypot(a1,b1), C2=Math.hypot(a2,b2), Cb=(C1+C2)/2;
  const Cb7=Math.pow(Cb,7), G=0.5*(1-Math.sqrt(Cb7/(Cb7+6103515625)));
  const a1p=(1+G)*a1, a2p=(1+G)*a2, C1p=Math.hypot(a1p,b1), C2p=Math.hypot(a2p,b2);
  let h1=Math.atan2(b1,a1p)/rad; if(h1<0)h1+=360; let h2=Math.atan2(b2,a2p)/rad; if(h2<0)h2+=360;
  const dLp=L2-L1, dCp=C2p-C1p; let dhp=0;
  if(C1p*C2p!==0){const d=h2-h1; dhp=Math.abs(d)<=180?d:(d>180?d-360:d+360);}
  const dHp=2*Math.sqrt(C1p*C2p)*Math.sin(dhp*rad/2);
  const Lbp=(L1+L2)/2, Cbp=(C1p+C2p)/2; let hbp=h1+h2;
  if(C1p*C2p!==0){hbp=Math.abs(h1-h2)<=180?(h1+h2)/2:((h1+h2<360)?(h1+h2+360)/2:(h1+h2-360)/2);}
  const T=1-0.17*Math.cos((hbp-30)*rad)+0.24*Math.cos(2*hbp*rad)+0.32*Math.cos((3*hbp+6)*rad)-0.20*Math.cos((4*hbp-63)*rad);
  const dth=30*Math.exp(-Math.pow((hbp-275)/25,2)), Cbp7=Math.pow(Cbp,7), Rc=2*Math.sqrt(Cbp7/(Cbp7+6103515625));
  const Sl=1+(0.015*Math.pow(Lbp-50,2))/Math.sqrt(20+Math.pow(Lbp-50,2)), Sc=1+0.045*Cbp, Sh=1+0.015*Cbp*T;
  const Rt=-Math.sin(2*dth*rad)*Rc;
  return Math.sqrt((dLp/Sl)**2+(dCp/Sc)**2+(dHp/Sh)**2+Rt*(dCp/Sc)*(dHp/Sh));
}

// ---- load wasm
const bytes = readFileSync(WASM);
let ex = null;
const fw = (fd,iov,c,np)=>{const v=new DataView(ex.memory.buffer);let t=0;for(let i=0;i<c;i++)t+=v.getUint32(iov+i*8+4,true);v.setUint32(np,t,true);return 0;};
const mod = await WebAssembly.instantiate(bytes,{wasi_snapshot_preview1:{proc_exit:()=>{throw 0},fd_close:()=>0,fd_seek:()=>0,fd_write:fw,fd_read:()=>0,environ_sizes_get:()=>0,environ_get:()=>0},env:{emscripten_notify_memory_growth:()=>{}}});
ex = mod.instance.exports; if (ex._initialize) ex._initialize(); ex.wasm_init();
ex.wasm_set_color_render_mode(MODE);

const rp = ex.malloc(SW*SH*3), cp = ex.malloc(28), pp = ex.malloc(PW*PH/2), ip = ex.malloc(PW*PH);
new Int32Array(ex.memory.buffer, cp, 7).set([0,100,100,100,100,100,30]);   // neutral adjust

function renderFlat(r,g,b){
  const h = new Uint8Array(ex.memory.buffer, rp, SW*SH*3);
  for (let i=0;i<SW*SH*3;i+=3){h[i]=r;h[i+1]=g;h[i+2]=b;}
  ex.wasm_set_color_render_mode(MODE);
  const rc = ex.wasm_dither_e6_15x(rp, PW, PH, cp, pp, ip);
  if (rc!==0) throw new Error('dither rc='+rc);
  const idx = new Uint8Array(ex.memory.buffer, ip, PW*PH);
  const frac = [0,0,0,0,0,0]; for (let i=0;i<idx.length;i++) frac[idx[i]]++;
  for (let i=0;i<6;i++) frac[i]/=idx.length;
  return frac;
}

// ---- test set
const SKIN = [[255,224,196],[241,194,167],[224,172,140],[198,134,98],[141,85,53],[235,180,150]];
const SAT  = {RED:[220,30,40],GREEN:[40,160,60],BLUE:[40,90,200],YELLOW:[240,210,30],ORANGE:[235,120,30],CYAN:[40,170,180],MAGENTA:[200,50,150]};
const MEM  = {sky:[120,170,230],grass:[90,150,70],foliage:[60,110,50]};
const GRAY = [32,64,96,128,160,192,224];

function score(rgb){
  const f = renderFlat(...rgb);
  const pred = ynPredict(f);
  const src = srgbToLab(...rgb);
  const dE = ciede2000(...pred, ...src);
  const Cs = Math.hypot(src[1],src[2]), Cp = Math.hypot(pred[1],pred[2]);
  const colored = f[2]+f[3]+f[4]+f[5];                 // yellow+red+blue+green
  const coloredRGB = f[3]+f[4]+f[5];                   // red+blue+green (the conspicuous speckle inks)
  const hSrc = (Math.atan2(src[2],src[1])*180/Math.PI+360)%360;
  const hPred= (Math.atan2(pred[2],pred[1])*180/Math.PI+360)%360;
  let hueErr = Math.abs(hPred-hSrc); if (hueErr>180) hueErr=360-hueErr;   // hue fidelity (°)
  return {f,pred,src,dE,Cs,Cp,chromaRet:Cs>5?Cp/Cs:1,colored,coloredRGB,L:pred[0],hSrc,hPred,hueErr};
}
// Hue wheel: 12 sRGB hues at fixed moderate saturation/lightness — pure hue-fidelity probe.
function hueWheel(){
  const out=[];
  for (let h=0; h<360; h+=30){
    const hr=h*Math.PI/180; const c=Math.cos(hr), s=Math.sin(hr);
    // build an sRGB-ish saturated colour at hue h via HSL(h,0.7,0.55)
    const H=h/60, X=1-Math.abs(H%2-1); let r,g,b;
    if(H<1){r=1;g=X;b=0;}else if(H<2){r=X;g=1;b=0;}else if(H<3){r=0;g=1;b=X;}
    else if(H<4){r=0;g=X;b=1;}else if(H<5){r=X;g=0;b=1;}else{r=1;g=0;b=X;}
    const L=0.55, a=0.7*(1-Math.abs(2*L-1)); const m=L-a/2;
    out.push([Math.round((r*a+m)*255),Math.round((g*a+m)*255),Math.round((b*a+m)*255),h]);
  }
  return out;
}

const fmt = n => (n>=0?' ':'')+n.toFixed(1);

function summary(verbose){
  let skinDE=0, skinSpeck=0;
  if (verbose) console.log('SKIN  (want: low ΔE, light L, low red/blu/grn speckle)');
  for (const s of SKIN){const r=score(s); skinDE+=r.dE; skinSpeck+=r.coloredRGB;
    if (verbose) console.log(`  rgb(${s.join(',')}) → ΔE${fmt(r.dE)}  predL${fmt(r.L)} srcL${fmt(r.src[0])}  R/B/G ${(r.coloredRGB*100).toFixed(0)}%  (R${(r.f[3]*100)|0} B${(r.f[4]*100)|0} G${(r.f[5]*100)|0} Y${(r.f[2]*100)|0} W${(r.f[1]*100)|0})`);}
  skinDE/=SKIN.length; skinSpeck/=SKIN.length;
  let satRet=0, satHue=0, n=0;
  if (verbose) console.log('\nSATURATED (want: high chromaRet ≤~1, low hueErr°)');
  for (const [k,s] of Object.entries(SAT)){const r=score(s); satRet+=r.chromaRet; satHue+=r.hueErr; n++;
    if (verbose) console.log(`  ${k.padEnd(8)} ΔE${fmt(r.dE)}  chromaRet ${r.chromaRet.toFixed(2)}  hueErr ${r.hueErr.toFixed(0)}°  (src h${r.hSrc.toFixed(0)}→pred h${r.hPred.toFixed(0)})`);}
  satRet/=n; satHue/=n;
  let whHue=0, whN=0;
  if (verbose) console.log('\nHUE WHEEL (hue fidelity at fixed sat/L)');
  for (const s of hueWheel()){const r=score(s); whHue+=r.hueErr; whN++;
    if (verbose) console.log(`  h${String(s[3]).padStart(3)} → predH ${r.hPred.toFixed(0)}  hueErr ${r.hueErr.toFixed(0)}°  chromaRet ${r.chromaRet.toFixed(2)}`);}
  whHue/=whN;
  if (verbose){console.log('\nMEMORY');
    for (const [k,s] of Object.entries(MEM)){const r=score(s); console.log(`  ${k.padEnd(8)} ΔE${fmt(r.dE)}  predL${fmt(r.L)} predC${fmt(r.Cp)}  (R${(r.f[3]*100)|0} B${(r.f[4]*100)|0} G${(r.f[5]*100)|0})`);}}
  let Ls=[], graySpeck=0;
  if (verbose) console.log('\nNEUTRAL RAMP');
  for (const v of GRAY){const r=score([v,v,v]); Ls.push(r.L); graySpeck+=r.coloredRGB;
    if (verbose) console.log(`  gray ${String(v).padStart(3)} → predL${fmt(r.L)}  colored ${(r.colored*100).toFixed(0)}% (R/B/G ${(r.coloredRGB*100).toFixed(0)}%)`);}
  graySpeck/=GRAY.length;
  return {skinDE, skinSpeck, satRet, satHue, whHue, graySpeck, drSpread:Math.max(...Ls)-Math.min(...Ls)};
}
function printSummary(m){
  console.log('=== SUMMARY ===');
  console.log(`  skin ΔE (faces)        : ${m.skinDE.toFixed(2)}     (lower better)`);
  console.log(`  skin R/B/G speckle     : ${(m.skinSpeck*100).toFixed(1)}%  (lower better)`);
  console.log(`  saturated chroma ret   : ${m.satRet.toFixed(2)}     (higher=vivid, ≤~1)`);
  console.log(`  saturated hue error    : ${m.satHue.toFixed(1)}°  (lower better — 色相还原)`);
  console.log(`  hue-wheel hue error    : ${m.whHue.toFixed(1)}°  (lower better)`);
  console.log(`  neutral colored speckle: ${(m.graySpeck*100).toFixed(1)}%  (lower better — 麻点)`);
  console.log(`  dynamic-range spread   : ${m.drSpread.toFixed(1)} L*  (higher better)`);
}

if (process.argv[3] === 'sweep' && MODE === 2 && ex.wasm_set_model_alpha) {
  // Numerically FIT α: objective J = skin speckle + neutral speckle (the two
  // failure modes) with a fidelity tie-break; report the table, pick the min.
  console.log(`\n=== α sweep (mode 2) — fit optical-integration fraction numerically ===`);
  console.log('  α     skinΔE  skinSpk%  satRet  neutSpk%  DR    J');
  let best=null;
  for (const a of [0.0,0.15,0.3,0.45,0.6,0.75,0.9,1.0]){
    ex.wasm_set_model_alpha(a);
    const m = summary(false);
    const J = m.skinSpeck*100 + m.graySpeck*100 + 0.3*m.skinDE - 0.2*m.drSpread; // lower better
    console.log(`  ${a.toFixed(2)}   ${m.skinDE.toFixed(1).padStart(5)}   ${(m.skinSpeck*100).toFixed(1).padStart(5)}    ${m.satRet.toFixed(2)}    ${(m.graySpeck*100).toFixed(1).padStart(5)}   ${m.drSpread.toFixed(0).padStart(3)}  ${J.toFixed(1)}`);
    if (!best || J < best.J) best = {a, J, m};
  }
  console.log(`\n  → best α = ${best.a}  (J=${best.J.toFixed(1)})`);
} else {
  if (ex.wasm_set_model_alpha && process.argv[3]) ex.wasm_set_model_alpha(parseFloat(process.argv[3]));
  console.log(`\n=== E6 scorecard (mode ${MODE}${MODE===2?' reflectance v2':MODE===0?' 6color':' mixpatch'}${ex.wasm_get_model_alpha?', α='+ex.wasm_get_model_alpha().toFixed(2):''}) ===\n`);
  printSummary(summary(true));
}
ex.free(rp);ex.free(cp);ex.free(pp);ex.free(ip);
