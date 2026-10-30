#pragma once

namespace esphome {
namespace access_control {

// Profile management page served at http://<device>/
static const char INDEX_HTML[] = R"=====(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Door Access Profiles</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--text:#1d2129;--muted:#667085;--line:#e3e6ea;--accent:#2563eb;--accent-text:#fff;
--ok:#15803d;--ok-bg:#e8f5ec;--bad:#b42318;--bad-bg:#fdecea;--chip:#eef1f5;--off:#98a2b3}
@media (prefers-color-scheme:dark){:root{--bg:#111418;--card:#1a1e24;--text:#e6e8eb;--muted:#9aa3ae;--line:#2a3038;
--accent:#3b82f6;--chip:#252b33;--ok:#4ade80;--ok-bg:#12291b;--bad:#f87171;--bad-bg:#2d1515;--off:#5b6470}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:860px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin:4px 0 16px}
h1{font-size:20px;margin:0}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:24px 0 8px}
.status{display:flex;align-items:center;gap:6px;color:var(--muted);font-size:13px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--off)}
.dot.on{background:var(--ok)}
.panel{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
input[type=text]{flex:1;min-width:160px;padding:9px 11px;border:1px solid var(--line);border-radius:8px;background:var(--bg);
color:var(--text);font:inherit}
button{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--text);border-radius:8px;padding:7px 12px;
cursor:pointer}
button:hover{border-color:var(--muted)}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--accent-text)}
button.danger{color:var(--bad)}
button.small{padding:4px 9px;font-size:13px}
button:disabled{opacity:.5;cursor:default}
#msg{display:none;margin-bottom:14px;padding:11px 14px;border-radius:10px;font-weight:500}
#msg.ok{display:block;background:var(--ok-bg);color:var(--ok)}
#msg.bad{display:block;background:var(--bad-bg);color:var(--bad)}
#msg .row{justify-content:space-between}
#hint{font-weight:400;font-size:13px;margin-top:4px;opacity:.85}
#hint:empty{display:none}
.profiles{display:grid;gap:12px}
.profile.disabled .pname{color:var(--muted);text-decoration:line-through}
.phead{display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap;margin-bottom:10px}
.pname{font-weight:600;font-size:17px}
.methods{display:grid;gap:10px}
.method{display:grid;grid-template-columns:70px 1fr;gap:10px;align-items:start;padding-top:10px;border-top:1px solid var(--line)}
.mlabel{color:var(--muted);font-size:13px;padding-top:4px}
.chips{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
.chip{display:inline-flex;align-items:center;gap:4px;background:var(--chip);border-radius:999px;padding:3px 4px 3px 11px;
font-size:13px;font-variant-numeric:tabular-nums}
.chip button{border:0;background:transparent;padding:0 6px;font-size:15px;line-height:1;color:var(--muted)}
.chip button:hover{color:var(--bad)}
.none{color:var(--muted);font-size:13px}
.toggle{display:inline-flex;align-items:center;gap:6px;font-size:13px;color:var(--muted);cursor:pointer}
.toggle input{width:16px;height:16px;accent-color:var(--accent)}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:6px 4px;border-top:1px solid var(--line)}
tr:first-child td{border-top:0}
.g{color:var(--ok);font-weight:600}.d{color:var(--bad);font-weight:600}
.muted{color:var(--muted)}
.empty{color:var(--muted);text-align:center;padding:22px}
@media (max-width:520px){.method{grid-template-columns:1fr;gap:4px}}
</style>
</head>
<body>
<main>
<header>
  <h1>Door Access Profiles</h1>
  <div class="status"><span id="fpdot" class="dot"></span><span id="fptext">Checking face/palm module...</span></div>
</header>

<div id="msg"><div class="row"><span id="msgtext"></span><button id="cancel" class="small" style="display:none">Cancel</button></div>
<div id="hint"></div></div>

<div class="panel">
  <form id="addform" class="row">
    <input id="newname" type="text" maxlength="23" placeholder="New profile name" autocomplete="off">
    <button class="primary" type="submit">Add profile</button>
  </form>
</div>

<h2>Profiles</h2>
<div id="profiles" class="profiles"></div>

<h2>Recent activity</h2>
<div class="panel"><table id="events"></table></div>

<h2>Maintenance</h2>
<div class="panel row" style="justify-content:space-between">
  <span class="muted">Erase every face and palm from the scanner (cards and PINs are kept).</span>
  <button id="wipe" class="danger small">Erase all faces &amp; palms</button>
</div>
</main>

<script>
const $=s=>document.querySelector(s);
let state=null, lastProfiles='', lastMsgSeq=-1, timer=null;
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

async function post(path, params){
  const body=new URLSearchParams(params||{});
  try{
    const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});
    const j=await r.json();
    if(!j.ok) showMsg(j.error||'Something went wrong.',false);
    refresh();
    return j.ok;
  }catch(e){showMsg('Lost connection to the door panel.',false);return false;}
}

function showMsg(text,ok){
  const m=$('#msg'); m.className=ok?'ok':'bad'; $('#msgtext').textContent=text;
}

function age(s){if(s<60)return s+'s ago';if(s<3600)return Math.floor(s/60)+'m ago';return Math.floor(s/3600)+'h ago';}

function chip(label,action){return `<span class="chip">${esc(label)}<button title="Remove" data-act="${action}">&times;</button></span>`;}

function renderProfiles(){
  const ps=state.profiles, L=state.limits, el=$('#profiles');
  if(!ps.length){el.innerHTML='<div class="panel empty">No profiles yet. Add one above.</div>';return;}
  const busy=state.enrol.kind!=='';
  el.innerHTML=ps.map(p=>{
    const cards=p.cards.length?p.cards.map(c=>chip(c,`card-rm:${p.id}:${c}`)).join(''):'<span class="none">None</span>';
    const faces=p.faces.length?p.faces.map((u,i)=>chip('Face '+(i+1),`bio-rm:${p.id}:${u}`)).join(''):'<span class="none">None</span>';
    const palms=p.palms.length?p.palms.map((u,i)=>chip('Palm '+(i+1),`bio-rm:${p.id}:${u}`)).join(''):'<span class="none">None</span>';
    const fpOff=!state.fp.online;
    return `<div class="panel profile ${p.enabled?'':'disabled'}">
      <div class="phead">
        <span class="pname">${esc(p.name)}</span>
        <div class="row">
          <label class="toggle"><input type="checkbox" data-act="enable:${p.id}" ${p.enabled?'checked':''}>Access allowed</label>
          <button class="small" data-act="rename:${p.id}">Rename</button>
          <button class="small danger" data-act="delete:${p.id}">Delete</button>
        </div>
      </div>
      <div class="methods">
        <div class="method"><span class="mlabel">Cards</span><div class="chips">${cards}
          <button class="small" data-act="enrol:card:${p.id}" ${busy||p.cards.length>=L.cards?'disabled':''}>+ Scan card</button>
          <button class="small" data-act="card-add:${p.id}" ${p.cards.length>=L.cards?'disabled':''}>+ Type number</button></div></div>
        <div class="method"><span class="mlabel">Face</span><div class="chips">${faces}
          <button class="small" data-act="enrol:face:${p.id}" ${busy||fpOff||p.faces.length>=L.faces?'disabled':''}>+ Add face</button></div></div>
        <div class="method"><span class="mlabel">Palm</span><div class="chips">${palms}
          <button class="small" data-act="enrol:palm:${p.id}" ${busy||fpOff||p.palms.length>=L.palms?'disabled':''}>+ Add palm</button></div></div>
        <div class="method"><span class="mlabel">PIN</span><div class="chips">
          ${p.pin?chip('PIN set',`pin-rm:${p.id}`):'<span class="none">None</span>'}
          <button class="small" data-act="enrol:pin:${p.id}" ${busy?'disabled':''} title="Type the PIN on the door panel">${p.pin?'Change PIN':'+ Set PIN'}</button>
          <button class="small" data-act="pin:${p.id}" ${busy?'disabled':''} title="Type the PIN in this browser">Type here</button></div></div>
      </div></div>`;
  }).join('');
}

function renderEvents(){
  const ev=state.events, t=$('#events');
  if(!ev.length){t.innerHTML='<tr><td class="empty">No activity since the panel started.</td></tr>';return;}
  t.innerHTML=ev.map(e=>`<tr><td class="${e.granted?'g':'d'}">${e.granted?'Granted':'Denied'}</td>
    <td>${esc(e.name)}</td><td class="muted">${esc(e.method)} &middot; ${esc(e.detail)}</td>
    <td class="muted" style="text-align:right;white-space:nowrap">${age(e.age)}</td></tr>`).join('');
}

function render(){
  $('#fpdot').className='dot'+(state.fp.online?' on':'');
  $('#fptext').textContent=state.fp.online?'Face/palm module online':'Face/palm module not responding';
  const key=JSON.stringify([state.profiles,state.enrol,state.fp.online]);
  if(key!==lastProfiles){lastProfiles=key;renderProfiles();}
  renderEvents();
  if(state.message.seq!==lastMsgSeq){
    lastMsgSeq=state.message.seq;
    if(state.message.text) showMsg(state.message.text,state.message.ok);
  }
  $('#cancel').style.display=state.enrol.kind?'inline-block':'none';
  $('#hint').textContent=state.enrol.hint?((state.enrol.kind==='pin'?'Panel: ':'Scanner: ')+state.enrol.hint):'';
  clearTimeout(timer);
  timer=setTimeout(refresh,state.enrol.kind?700:2000);
}

async function refresh(){
  try{
    const r=await fetch('/api/state',{cache:'no-store'});
    state=await r.json();
    render();
  }catch(e){
    $('#fptext').textContent='Can\'t reach the door panel - retrying...';
    clearTimeout(timer); timer=setTimeout(refresh,3000);
  }
}

$('#addform').addEventListener('submit',async ev=>{
  ev.preventDefault();
  const name=$('#newname').value.trim();
  if(!name) return;
  if(await post('/api/profile/add',{name})) $('#newname').value='';
});

$('#cancel').addEventListener('click',()=>post('/api/enrol/cancel'));

$('#wipe').addEventListener('click',()=>{
  if(confirm('Erase ALL faces and palms from the scanner and remove them from every profile?\nCards and PINs are kept.'))
    post('/api/fp/wipe');
});

document.addEventListener('click',ev=>{
  const b=ev.target.closest('button[data-act]'); if(!b) return;
  const [act,a,c]=b.dataset.act.split(':');
  const nameOf=id=>(state.profiles.find(p=>p.id==id)||{}).name||'';
  if(act==='delete'){
    if(confirm(`Delete profile "${nameOf(a)}"?\nIts cards, PIN, faces and palms will stop working.`)) post('/api/profile/delete',{id:a});
  }else if(act==='rename'){
    const n=prompt('New name',nameOf(a)); if(n&&n.trim()) post('/api/profile/update',{id:a,name:n.trim()});
  }else if(act==='enrol'){
    post('/api/enrol',{kind:a,id:c});
  }else if(act==='card-add'){
    const n=prompt('Card number (as shown in the activity list)'); if(n&&n.trim()) post('/api/card/add',{id:a,card:n.trim()});
  }else if(act==='card-rm'){
    if(confirm(`Remove card ${c} from ${nameOf(a)}?`)) post('/api/card/remove',{id:a,card:c});
  }else if(act==='bio-rm'){
    if(confirm(`Remove this from ${nameOf(a)}? It will also be erased from the scanner.`)) post('/api/bio/remove',{id:a,uid:c});
  }else if(act==='pin'){
    const n=prompt('New PIN for '+nameOf(a)+' (4-8 digits)'); if(n!==null&&n.trim()) post('/api/pin',{id:a,pin:n.trim()});
  }else if(act==='pin-rm'){
    if(confirm(`Remove ${nameOf(a)}'s PIN?`)) post('/api/pin',{id:a,pin:''});
  }
});

document.addEventListener('change',ev=>{
  const i=ev.target.closest('input[data-act]'); if(!i) return;
  const id=i.dataset.act.split(':')[1];
  post('/api/profile/update',{id,enabled:i.checked?'1':'0'});
});

refresh();
</script>
</body>
</html>
)=====";

}  // namespace access_control
}  // namespace esphome
