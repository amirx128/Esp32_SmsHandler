#include "WebUI.h"

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en" dir="ltr">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>Device Settings</title>
  <style>
    :root { --p:#3498db; --d:#e74c3c; --s:#2ecc71; --bg:#1a1a1a; --card:#2d2d2d; --text:#eee; }
    body { font-family: Tahoma; background:var(--bg); color:var(--text); margin:0; }
    .container { max-width: 1100px; margin: 20px auto; padding: 20px; }
    .card { background:var(--card); border-radius:12px; padding:20px; margin-bottom:20px; box-shadow:0 4px 12px rgba(0,0,0,0.3); }
    .card.remote-active { box-shadow:0 0 0 3px var(--s) inset,0 4px 12px rgba(0,0,0,0.3); transition: box-shadow .3s ease; }
    h1,h2 { text-align:center; color:var(--p); }
    input, select, button { padding:10px; margin:5px 0; border-radius:8px; width:100%; border:1px solid #555; background:rgba(255,255,255,0.1); color:var(--text); }
    button { background:var(--p); color:#fff; border:none; cursor:pointer; font-weight:bold; }
    .btn-d { background:var(--d); }
    .btn-s { background:var(--s); }
    .grid { display:grid; grid-template-columns: repeat(auto-fill, minmax(280px,1fr)); gap:15px; }
    .key-card { border:1px solid #444; border-radius:8px; padding:15px; }
    .system { border-left:5px solid var(--p); }
    .hidden { display:none; }
    .error { color:var(--d); font-size:0.9em; }
    .mobile-input { direction:ltr; text-align:left; font-family:monospace; }
    table { width:100%; border-collapse: collapse; font-size: 0.9rem; }
    th, td { border-bottom:1px solid #444; padding:8px 6px; text-align:right; }
    th { position: sticky; top: 0; background:#222; }
    .table-wrap { max-height: 420px; overflow:auto; border:1px solid #444; border-radius:8px; }
    .mono { direction:ltr; text-align:left; font-family:monospace; }
    .badge { padding:3px 8px; border-radius:8px; font-size:0.8rem; }
    .q { background:#555; }
    .s { background:#2c7; }
    .f { background:#c44; }
    .modal { position:fixed; top:0; left:0; width:100%; height:100%; background:rgba(0,0,0,0.7); display:flex; align-items:center; justify-content:center; z-index:1000; }
    .modal.hidden { display:none; }
    .modal-content { background:#2d2d2d; padding:20px; border-radius:12px; width:90%; max-width:900px; max-height:90%; overflow:auto; box-shadow:0 10px 25px rgba(0,0,0,0.5); }
    .modal-close { float:right; font-size:1.5rem; cursor:pointer; }
    .btn-row { display:grid; grid-template-columns: repeat(auto-fit,minmax(220px,1fr)); gap:10px; }
    .loading { text-align:center; padding:20px; color:#ccc; }
    .spinner { margin:0 auto 10px; width:32px; height:32px; border:3px solid #555; border-top-color: var(--p); border-radius:50%; animation: spin 1s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }
  </style>
</head>
<body>

<div id="loginBox" class="container">
  <div class="card" style="max-width:420px;margin:auto;">
    <h2>System Login</h2>
    <input type="text" id="user" placeholder="Username" />
    <input type="password" id="pass" placeholder="Password" />
    <button type="button" id="loginBtn">Login</button>
    <p class="error" id="err"></p>
  </div>
</div>

<div id="main" class="container hidden">
  <div class="card" id="nodeBanner">
    <h1 id="deviceTitle">Device Settings <span id="nodeNameLabel" style="font-size:0.7em;color:#ccc;"></span></h1>
    <label>Device Time</label>
    <input type="datetime-local" id="deviceTime" step="1" />
    <div class="btn-row">
      <button onclick="syncTime()">Sync Time</button>
      <button onclick="sendTestSms()">Send Test SMS</button>
      <button class="btn-s" onclick="toggleHelp()">Help</button>
      <button class="btn-s" onclick="changePass()">Change Password</button>
      <button class="btn-d" onclick="factoryReset()">Factory Reset</button>
      <button class="btn-d" onclick="ResetEsp()">Restart Device</button>
      <button class="btn-d" onclick="logout()">Logout</button>
    </div>
  </div>

  <div class="card">
    <h2>Tools & Logs</h2>
    <div class="btn-row">
      <button onclick="openModal('wifiModal')">Mesh Wi-Fi</button>
      <button onclick="openModal('nodesModal')">Mesh Nodes</button>
      <button onclick="openModal('smsModal')">SMS Log</button>
      <button onclick="openModal('inboxModal')">Inbox</button>
      <button onclick="openModal('gasModal')">Gas Status</button>
      <button onclick="openModal('dhtModal')">Temp & Humidity</button>
      <button onclick="openModal('memModal')">Memory Usage</button>
    </div>
  </div>

  <div id="grid" class="grid"></div>
</div>

<!-- Modals -->
<div id="wifiModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('wifiModal')">&times;</span>
    <h3>Mesh Network (elixMesh)</h3>
    <label for="meshSsid">Mesh Name</label>
    <input type="text" id="meshSsid" placeholder="Mesh name (default: elixMesh)"/>
    <label for="meshPass">Mesh Password</label>
    <input type="password" id="meshPass" placeholder="Mesh password (default: 12345678)"/>
    <label style="display:flex;align-items:center;gap:6px;"><input type="checkbox" id="meshPassShow" onchange="toggleMeshPass()"/> <span>Show password</span></label>
    <button onclick="saveMeshWifi()">Save Mesh Wi-Fi</button>
    <p class="error" id="meshMsg"></p>
  </div>
</div>

<div id="meshModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeMeshModal()">&times;</span>
    <h3 id="meshModalTitle">Node Messages</h3>
    <div class="table-wrap">
      <table>
        <thead><tr><th>Message ID</th><th>Type</th><th>Payload</th><th>Sent At</th><th>Acks</th></tr></thead>
        <tbody id="meshModalBody"><tr><td colspan="5" style="text-align:center;color:#999;">No messages</td></tr></tbody>
      </table>
    </div>
  </div>
</div>

<div id="nodesModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('nodesModal')">&times;</span>
    <h3>Mesh Nodes</h3>
    <div class="table-wrap">
      <table>
        <thead><tr><th>Name</th><th>Node ID</th><th>MAC</th><th>SSID</th><th>Status</th><th>Time</th><th>Actions</th></tr></thead>
        <tbody id="mesh-nodes-body"><tr><td colspan="7" style="text-align:center;color:#999;">Loading...</td></tr></tbody>
      </table>
    </div>
  </div>
</div>

<div id="smsModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('smsModal')">&times;</span>
    <h3>SMS Log (last 200)</h3>
    <div class="table-wrap">
      <table>
        <thead><tr><th>ID</th><th>Number</th><th>Reason</th><th>Sent At</th><th>Text</th><th>Status</th></tr></thead>
        <tbody id="sms-tbody"><tr><td colspan="6" style="text-align:center;color:#999;">Loading...</td></tr></tbody>
      </table>
    </div>
  </div>
</div>

<div id="inboxModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('inboxModal')">&times;</span>
    <h3>Inbox (last 200)</h3>
    <div class="table-wrap">
      <table>
        <thead><tr><th>ID</th><th>From</th><th>Received At</th><th>Text</th></tr></thead>
        <tbody id="inbox-tbody"><tr><td colspan="4" style="text-align:center;color:#999;">Loading...</td></tr></tbody>
      </table>
    </div>
  </div>
</div>

<div id="gasModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('gasModal')">&times;</span>
    <h3>Gas Status (MQ)</h3>
    <p><strong>Current:</strong> <span id="gasVal">-</span></p>
    <p><small>Range: <span id="gasRange">-</span></small></p>
  </div>
</div>

<div id="dhtModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('dhtModal')">&times;</span>
    <h3>Temperature & Humidity (DHT)</h3>
    <p><strong>Temperature:</strong> <span id="dhtTemp">-</span> °C</p>
    <p><strong>Humidity:</strong> <span id="dhtHum">-</span> %</p>
    <p><small>Temp range: <span id="dhtRange">-</span></small></p>
  </div>
</div>

<div id="memModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('memModal')">&times;</span>
    <h3>Memory Usage</h3>
    <p>Total: <span id="memTotalKB">-</span> KB</p>
    <p>Used: <span id="memUsedKB">-</span> KB</p>
    <p>Free: <span id="memFreeKB">-</span> KB</p>
    <p>Used %: <span id="memPct">-</span>%</p>
  </div>
</div>

<div id="helpModal" class="modal hidden">
  <div class="modal-content">
    <span class="modal-close" onclick="closeModal('helpModal')">&times;</span>
    <h3>Commands Help</h3>
    <div class="table-wrap">
      <table>
        <thead><tr><th>Command</th><th>Description</th></tr></thead>
        <tbody id="help-tbody"><tr><td colspan="2" style="text-align:center;color:#999;">Loading...</td></tr></tbody>
      </table>
    </div>
  </div>
</div>

<script>
let TOKEN = null;
let meshState = {nodes:[], events:[], states:[]};
let currentNodeId = null;
let currentNodeName = null;
let isEditing = false;
let lastMeshAutoFetch = 0;
let keysLoading = false;
let keysNodeId = null;
let remoteClockBaseMs = null;
let remoteClockStart = null;

function $(id){ return document.getElementById(id); }

const HELP = [
  {cmd:'help', desc:'Show list of SMS commands'},
  {cmd:'check', desc:'Send device status report'},
  {cmd:'alron/alrof', desc:'Enable/disable alarm monitoring'},
  {cmd:'piron/pirof', desc:'Enable/disable PIR sensor'},
  {cmd:'vibon/vibof', desc:'Enable/disable vibration sensor'},
  {cmd:'gasOn/gasOff', desc:'Enable/disable gas sensor'},
  {cmd:'ledon/ledof', desc:'Enable/disable LED'},
  {cmd:'buzon/buzof', desc:'Enable/disable buzzer'},
  {cmd:'smson/smsof', desc:'Enable/disable SMS alerts'},
  {cmd:'wifon/wifof', desc:'Enable/disable WiFi (SoftAP)'},
  {cmd:'YYYYMMDD:HHmm', desc:'Set device date/time (e.g. 14040816:1221)'}
];

function renderHelp(){
  const tb = $('help-tbody');
  if (!tb) return;
  tb.innerHTML = '';
  HELP.forEach(row => {
    const tr = document.createElement('tr');
    tr.innerHTML = `<td class="mono">${row.cmd}</td><td>${row.desc}</td>`;
    tb.appendChild(tr);
  });
}

function setDeviceTimeInput(tsMs){
  const input = $('deviceTime');
  if (!input) return;
  const d = tsMs ? new Date(tsMs) : new Date();
  const iso = new Date(d.getTime() - d.getTimezoneOffset()*60000).toISOString().slice(0,19);
  input.value = iso;
}

async function api(path, data=null){
  try{
    if (!TOKEN) TOKEN = localStorage.getItem('TOKEN');
    const res = await fetch('/api'+path, {
      method: data ? 'POST' : 'GET',
      headers: { 'Content-Type': 'application/json', ...(TOKEN?{'X-Auth':TOKEN}:{}) },
      body: data ? JSON.stringify(data) : null
    });
    return await res.json();
  }catch(e){
    const errEl = document.getElementById('err');
    if (errEl) errEl.textContent = 'Connection error';
    return { success:false, error:'Network Error' };
  }
}

async function login(){

  console.log('[WEB] login start..... ');
  const userEl = $('user'), passEl = $('pass');

  if (!userEl || !passEl){ alert('Login error: input elements not found'); return; }
  const user = userEl.value.trim();
  const pass = passEl.value;
  if (!user || !pass){ $('err').textContent = '                                       '; return; }

  console.log('[WEB] login start user=', user);
  const res = await api('/login', { user, pass });
  if (res.success){
    console.log('[WEB] login success user=', user);
    TOKEN = res.token;
    localStorage.setItem('TOKEN', TOKEN);
    await afterLogin();
  }else{
    console.warn('[WEB] login failed user=', user, 'err=', res.error);
    $('err').textContent = res.error || 'Invalid username or password';
  }
}

async function tryResumeSession(){
  const stored = localStorage.getItem('TOKEN');
  if (!stored) return;
  TOKEN = stored;
  const probe = await api('/keys');
  if (probe && probe.keys){
    await afterLogin(probe);
  }else{
    TOKEN = null;
    localStorage.removeItem('TOKEN');
  }
}

async function afterLogin(preloaded){
  $('loginBox').classList.add('hidden');
  $('main').classList.remove('hidden');
  setDeviceTimeInput(Date.now());
  await autoSyncClock();
  setInterval(autoSyncClock, 5 * 60 * 1000);
  currentNodeId = null;
  currentNodeName = null;

  if (preloaded && preloaded.keys){
    window.keys = [];
    preloaded.keys.forEach(k => window.keys.push(k));
    populateMeshSettings(window.keys);
    render();
  }else{
    await loadKeys();
  }
  await loadSmsLog();
  await loadInboxLog();
  await loadMeshState();
  await loadMem();
  if (typeof loadGas === 'function') await loadGas();
  if (typeof loadDht === 'function') await loadDht();
  setInterval(loadSmsLog, 6000);
  setInterval(loadInboxLog, 8000);
  setInterval(loadMeshState, 10000);
  setInterval(loadMem, 12000);
  if (typeof loadGas === 'function') setInterval(loadGas, 4000);
  if (typeof loadDht === 'function') setInterval(loadDht, 4000);
  setInterval(updateRemoteClockUi, 1000);
}

function logout(){
  $('main').classList.add('hidden');
  $('loginBox').classList.remove('hidden');
  $('user').value = $('pass').value = '';
  $('err').textContent = '';
  TOKEN = null;
  localStorage.removeItem('TOKEN');
}

// Hook buttons & expose handlers
const loginBtn = document.getElementById('loginBtn');
if (loginBtn) loginBtn.onclick = login;
window.login = login;
window.logout = logout;

async function autoSyncClock(){
  try{
    const now = Date.now();
    await api('/setTime', { timestamp: now });
    setDeviceTimeInput(now);
  }catch(e){ }
}

async function loadKeys(){
  const data = await api('/keys');
  if (data.keys){
    window.keys = [];
    data.keys.forEach(k => window.keys.push(k));
    populateMeshSettings(window.keys);
    render();
    keysLoading = false;
  }
}

function inputFor(k){
  const val = k.value || '';
  if (k.type==='dropdown' || k.type==='bool'){
    return `<select id="i-${k.key}" onfocus="isEditing=true" onblur="isEditing=false">${k.options.split(',').map(o=>`<option value="${o}" ${val===o?'selected':''}>${o}</option>`).join('')}</select>`;
  }else if (k.type==='mobile'){
    return `<input class="mobile-input" id="i-${k.key}" value="${val}" maxlength="11" placeholder="09121234567" onfocus="isEditing=true" onblur="isEditing=false"/>`;
  }else if (k.type==='int'){
    return `<input type="number" id="i-${k.key}" value="${val}" min="${k.min}" max="${k.max}" onfocus="isEditing=true" onblur="isEditing=false"/>`;
  }else{
    return `<input type="text" id="i-${k.key}" value="${val}" maxlength="${k.max||''}" placeholder="min ${k.min} chars" onfocus="isEditing=true" onblur="isEditing=false"/>`;
  }
}

function render(){
  const g = $('grid');
  if (!g){ console.warn('         #grid                !'); return; }
  g.innerHTML = '';
  if (keysLoading && (!window.keys || !window.keys.length)){
    g.innerHTML = '<div class="loading"><div class="spinner"></div><p>Loading keys...</p></div>';
    return;
  }
  if (!window.keys || !window.keys.length){
    g.innerHTML = '<div class="loading"><p>No keys loaded. Fetch a node or reload.</p></div>';
    return;
  }
  window.keys.forEach(k => {
    const div = document.createElement('div');
    div.className = 'key-card' + (k.isSystem?' system':'');
    div.innerHTML = `
      <strong>${k.key}</strong>${k.isSystem?' (system)':''}
      <div>${inputFor(k)}</div>
      <button onclick="save('${k.key}')">Save</button>
      <p class="error" id="e-${k.key}"></p>
    `;
    g.appendChild(div);
  });
}

function isRemoteSelected(){
  if (!currentNodeId || !meshState.nodes) return false;
  const target = currentNodeId.toUpperCase();
  const me = meshState.nodes.find(n => n.local);
  if (!me) return true;
  return target !== (me.id||'').toUpperCase() &&
         target !== (me.mac||'').toUpperCase() &&
         target !== (me.friendly||me.id||'').toUpperCase();
}

function setDisplay(selector, show){
  document.querySelectorAll(selector).forEach(el => {
    el.style.display = show ? '' : 'none';
  });
}

function updateControlVisibility(){
  const remote = isRemoteSelected();
  const hideRemote = [
    'button[onclick*="syncTime"]',
    'button[onclick*="sendTestSms"]',
    'button[onclick*="toggleHelp"]',
    'button[onclick*="changePass"]',
    'button[onclick*="factoryReset"]',
    'button[onclick*="logout"]',
    'button[onclick*="wifiModal"]',
    'button[onclick*="smsModal"]',
    'button[onclick*="inboxModal"]',
    'button[onclick*="gasModal"]',
    'button[onclick*="dhtModal"]',
    'button[onclick*="memModal"]'
  ];
  const showAlways = [
    'button[onclick*="ResetEsp"]',
    'button[onclick*="nodesModal"]'
  ];
  hideRemote.forEach(sel => setDisplay(sel, !remote));
  showAlways.forEach(sel => setDisplay(sel, true));
}

function updateRemoteClockBase(){
  if (!isRemoteSelected()){ remoteClockBaseMs = null; remoteClockStart = null; return; }
  const target = currentNodeId.toUpperCase();
  let ts = null;
  const st = (meshState.states||[]).find(s => (s.id||'').toUpperCase() === target);
  if (st && st.raw && st.raw.t) ts = st.raw.t;
  if (!ts){
    const node = (meshState.nodes||[]).find(n => (n.id||'').toUpperCase() === target || (n.mac||'').toUpperCase() === target);
    if (node && node.timeMs) ts = node.timeMs;
  }
  if (ts){
    remoteClockBaseMs = ts;
    remoteClockStart = Date.now();
  }
}

function updateRemoteClockUi(){
  const input = $('deviceTime');
  if (!input) return;
  if (remoteClockBaseMs && remoteClockStart){
    const delta = Date.now() - remoteClockStart;
    const ts = remoteClockBaseMs + delta;
    const d = new Date(ts);
    const iso = new Date(d.getTime() - d.getTimezoneOffset()*60000).toISOString().slice(0,19);
    input.value = iso;
  }
}

function populateMeshSettings(keys){
  const ssidKey = keys.find(k => k.key === 'mesh_name');
  const passKey = keys.find(k => k.key === 'mesh_pass');
  const ssidInput = $('meshSsid');
  const passInput = $('meshPass');
  if (ssidInput && ssidKey) ssidInput.value = ssidKey.value || '';
  if (passInput && passKey) passInput.value = passKey.value || '';
}

async function saveMeshWifi(){
  const msgEl = $('meshMsg');
  if (msgEl) msgEl.textContent = '';
  const ssidEl = $('meshSsid');
  const passEl = $('meshPass');
  const ssid = ssidEl ? ssidEl.value.trim() : '';
  const pass = passEl ? passEl.value : '';
  let res1 = await api('/save', {key: 'mesh_name', value: ssid});
  let res2 = await api('/save', {key: 'mesh_pass', value: pass});
  if (msgEl){
    if (res1.success && res2.success) msgEl.textContent = 'Saved mesh Wi-Fi settings.';
    else msgEl.textContent = (res1.error || res2.error || 'Save error');
  }
}

function toggleMeshPass(){
  const inp = $('meshPass');
  if (!inp) return;
  const show = $('meshPassShow') && $('meshPassShow').checked;
  inp.type = show ? 'text' : 'password';
}

async function save(key){
  const val = document.getElementById('i-'+key).value;
  let res;
  if (currentNodeId){
    res = await api('/mesh/saveRemote', {nodeId: currentNodeId, key, value: val});
    // refresh remote keys shortly after save
    if (keysNodeId !== (currentNodeId || '').toUpperCase()) keysLoading = true;
    setTimeout(()=>loadMeshKeys(currentNodeId), 800);
  }else{
    res = await api('/save', {key, value: val});
  }
  if (res.success) { $('e-'+key).textContent = ''; }
  else { $('e-'+key).textContent = res.error || 'Error'; }
}

async function factoryReset(){
  if (confirm('                       ')){
    await api('/reset', {});
    loadKeys();
    loadSmsLog();
  }
}

async function changePass(){
  const oldp = prompt('Current password:');
  if (!oldp) return;
  const newp = prompt('New password (min 4 chars):');
  if (!newp || newp.length < 4) return alert('Password too short');
  const res = await api('/changepass', {old: oldp, new: newp});
  alert(res.success ? 'Password changed' : res.error);
}

function toggleHelp(){
  renderHelp();
  openModal('helpModal');
}

async function sendTestSms(){
  const res = await api('/sendTestSms', {});
  alert(res && res.success ? 'Test SMS enqueued' : (res.error || 'Failed'));
}

async function ResetEsp(){
  if (currentNodeId){
    await api('/mesh/resetRemote', { nodeId: currentNodeId });
  }else{
    await api('/ResetEsp', {});
  }
}

async function sendTestSms(){
  const res = await api('/sendTestSms', {});
  alert(res && res.success ? 'Test SMS enqueued' : (res.error || 'Failed'));
}

function syncTime(){
  const input = document.getElementById('deviceTime');
  const selected = new Date(input.value);
  if (isNaN(selected.getTime())) return alert('Invalid time');
  api('/setTime', { timestamp: selected.getTime() }).then(res => {
    alert(res.success ? 'Device time updated' : res.error);
  });
}

function fmtTs(ms){
  try{
    if (!ms || ms < 1000000000000) return '-';
    const d = new Date(ms);
    return d.toLocaleString('en-US');
  }catch(e){ return ms; }
}

async function loadSmsLog(){
  const data = await api('/smsLog');
  const tb = $('sms-tbody');
  if (!tb) return;
  tb.innerHTML = '';
  if (!data.success || !data.items || !data.items.length){
    tb.innerHTML = `<tr><td colspan="6" style="text-align:center;color:#999;">                        </td></tr>`;
    return;
  }
  data.items.forEach(row=>{
    const tr = document.createElement('tr');
    const st = (row.status === 'delivered' || row.status === 'sent') ? 's'
               : ((row.status === 'queued' || row.status === 'sending') ? 'q' : 'f');
    tr.innerHTML = `
      <td class="mono">${row.id}</td>
      <td class="mono">${row.number}</td>
      <td>${row.reason||''}</td>
      <td class="mono">${fmtTs(row.timestamp)}</td>
      <td style="white-space:nowrap; max-width:420px; overflow:hidden; text-overflow:ellipsis;" title="${row.text}">${row.text}</td>
      <td><span class="badge ${st}">${row.status}</span></td>
    `;
    tb.appendChild(tr);
  });
}

async function loadInboxLog(){
  const data = await api('/inboxLog');
  const tb = $('inbox-tbody');
  if (!tb) return;
  tb.innerHTML = '';
  if (!data.success || !data.items || !data.items.length){
    tb.innerHTML = `<tr><td colspan="4" style="text-align:center;color:#999;">                        </td></tr>`;
    return;
  }
  data.items.forEach(row=>{
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td class="mono">${row.id}</td>
      <td class="mono">${row.from}</td>
      <td class="mono">${fmtTs(row.timestamp)}</td>
      <td style="white-space:nowrap; max-width:420px; overflow:hidden; text-overflow:ellipsis;" title="${row.text}">${row.text}</td>
    `;
    tb.appendChild(tr);
  });
}

async function loadMem(){
  const data = await api('/mem');
  if (!data || !data.success) return;
  if ($('memTotalKB')) $('memTotalKB').textContent = data.totalKB;
  if ($('memFreeKB'))  $('memFreeKB').textContent  = data.freeKB;
  if ($('memUsedKB'))  $('memUsedKB').textContent  = data.usedKB;
  if ($('memPct'))     $('memPct').textContent     = data.pct;
}

async function loadGas(){
  const data = await api('/gas');
  if (!data || !data.success) return;
  if ($('gasVal')) $('gasVal').textContent = data.value;
  if ($('gasRange')) $('gasRange').textContent = `${data.min}..${data.max}`;
}

async function loadDht(){
  const data = await api('/dht');
  if (!data || !data.success) return;
  if ($('dhtTemp')) $('dhtTemp').textContent = data.t;
  if ($('dhtHum')) $('dhtHum').textContent = data.h;
  if ($('dhtRange')) $('dhtRange').textContent = `T: ${data.min}..${data.max}, H: ${data.hmin}..${data.hmax}`;
}

async function loadMeshState(){
  console.log('[WEB] loadMeshState start');
  const data = await api('/mesh/state');
  if (!data || !data.nodes){ console.warn('[WEB] loadMeshState empty/invalid', data); return; }
  console.log('[WEB] loadMeshState got', (data.nodes||[]).length, 'nodes', (data.keys||[]).length, 'keys');

  // اگر کلید/استیت نداریم و نود غیرلوکال داریم، خودکار درخواست بفرست
  // Auto mesh fetch disabled; user must click Fetch State for remote nodes

  meshState = data;
  updateControlVisibility();
  updateRemoteClockBase();
  // Try to refresh keys via lightweight endpoint for current remote node (بدون تغییر وضعیت لودینگ)
  if (currentNodeId){
    setTimeout(()=>loadMeshKeys(currentNodeId), 300);
  }
   // Update current node name label
  const label = $('nodeNameLabel');
  if (label){
    const nid = currentNodeId;
    let friendly = '';
    if (nid){
      const st = (data.states||[]).find(s => s.id === nid);
      const kn = (data.keys||[]).find(s => s.id === nid);
      friendly = (st && st.friendly) ? st.friendly : ((kn && kn.friendly) ? kn.friendly : nid);
    }else{
      const me = data.nodes.find(n => n.local);
      friendly = me ? (me.friendly || me.id) : '';
    }
    label.textContent = friendly ? `(${friendly})` : '';
  }
  // Apply remote keys if selected
  let applied = false;
  const target = (currentNodeId || '').toUpperCase();
  if (currentNodeId){
    // If requesting self, fall back to local keys immediately
    const me = data.nodes.find(n => n.local);
    const isSelf = me && (
      target === (me.id||'').toUpperCase() ||
      target === (me.mac||'').toUpperCase() ||
      target === (me.friendly||me.id||'').toUpperCase()
    );
    if (isSelf){
      await loadKeys();
      applied = true;
    }else{
      const rk = (data.keys||[]).find(k =>
        target === (k.id||'').toUpperCase() ||
        target === (k.sid||'').toUpperCase() ||
        target === (k.friendly||'').toUpperCase() ||
        (k.raw && (
          target === (k.raw.sid||'').toUpperCase() ||
          target === (k.raw.id||'').toUpperCase() ||
          target === (k.raw.friendly||'').toUpperCase()
        ))
      );
      if (rk && rk.raw && rk.raw.keys){
        window.keys = [];
        rk.raw.keys.forEach(k => window.keys.push(k));
        applied = true;
      }
      else if (rk && rk.raw && rk.raw.p === 0 && rk.raw.total){ // partial page arrived
        window.keys = [];
        if (rk.raw.keys) rk.raw.keys.forEach(k => window.keys.push(k));
        applied = true;
      }
    }
  }
  if (!applied && !window.keys){
    // If keys not present, try fetching lightweight keys endpoint
    if (currentNodeId) {
      keysLoading = true;
      setTimeout(()=>loadMeshKeys(currentNodeId), 500);
    }
  }
  if (applied && !isEditing) render();
  const tb = $('mesh-nodes-body');
  if (tb){
    tb.innerHTML = '';
    if (!data.nodes.length){
      tb.innerHTML = `<tr><td colspan="7" style="text-align:center;color:#999;">No nodes detected</td></tr>`;
    }else{
      data.nodes.forEach(node => {
        const friendly = node.friendly || node.id;
        const me = node.local ? ' (me)' : '';
        let status = node.online ? 'online' : 'offline';
        let cls = node.online ? 's' : 'f';
        if (node.authError) { status = 'auth error'; cls = 'f'; }
        const nodeTime = (node.timeMs && node.timeMs > 0) ? fmtTs(node.timeMs) : '-';
        const tr = document.createElement('tr');
        tr.innerHTML = `
          <td>${friendly}${me}</td>
          <td class="mono">${node.id}</td>
          <td class="mono">${node.mac || '-'}</td>
          <td class="mono">${node.ssid || '-'}</td>
          <td><span class="badge ${cls}">${status}</span></td>
          <td class="mono">${nodeTime}</td>
          <td><div class="btn-row" style="grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:6px;">
                <button onclick="openMeshModal('${node.id}')">Messages</button>
                <button onclick="requestState('${node.mac || node.id}')">Fetch State</button>
              </div></td>`;
        tb.appendChild(tr);
      });
    }
  }
}

async function loadMeshKeys(nodeId){
  try{
    const qs = nodeId ? (`?id=${encodeURIComponent(nodeId)}`) : '';
    const data = await api('/mesh/keys' + qs);
    if (data && data.keys && data.keys.length){
      const rk = nodeId ? data.keys.find(k =>
        nodeId.toUpperCase() === (k.id||'').toUpperCase() ||
        nodeId.toUpperCase() === (k.sid||'').toUpperCase() ||
        nodeId.toUpperCase() === (k.friendly||'').toUpperCase()
      ) : data.keys[0];
      if (rk && rk.raw && rk.raw.keys){
        window.keys = [];
        rk.raw.keys.forEach(k => window.keys.push(k));
        keysNodeId = nodeId ? nodeId.toUpperCase() : null;
      }
    }
  }catch(e){
    console.warn('loadMeshKeys error', e);
  }finally{
    keysLoading = false;
    if (!isEditing) render();
  }
}

function openMeshModal(nodeId){
  closeModal('nodesModal'); // ensure only one modal visible
  const modal = $('meshModal');
  if (!modal) return;
  modal.classList.remove('hidden');
  const title = $('meshModalTitle');
  const node = meshState.nodes ? meshState.nodes.find(n => n.id === nodeId) : null;
  if (title) title.textContent = node ? `Messages from ${node.friendly || node.id}${node.local?' (me)':''}` : `Messages (${nodeId})`;
  const body = $('meshModalBody');
  if (!body) return;
  body.innerHTML = '';
  const allEvents = meshState.events || [];
  const filtered = allEvents.filter(evt => (evt.originId && evt.originId === nodeId) || (!evt.originId && node && evt.origin === (node.friendly || node.id)));
  if (!filtered.length){
    body.innerHTML = `<tr><td colspan="5" style="text-align:center;color:#999;">No messages</td></tr>`;
    return;
  }
  filtered.forEach(evt => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td class="mono">${evt.id}</td>
      <td>${evt.type}</td>
      <td>${evt.payload}</td>
      <td class="mono">${fmtTs(evt.ts)}</td>
      <td>${(evt.acks||[]).join(', ') || '-'}</td>`;
    body.appendChild(tr);
  });
}

async function requestState(nodeId){
  currentNodeId = nodeId;
  currentNodeName = nodeId;
  closeModal('nodesModal');
  const prevNode = keysNodeId;
  const upId = nodeId ? nodeId.toUpperCase() : null;
  if (!prevNode || prevNode !== upId){
    window.keys = [];
    keysLoading = true; // فقط وقتی نود عوض شده است
  }
  render();
  remoteClockBaseMs = null;
  remoteClockStart = null;
  const banner = $('nodeBanner');
  if (banner){
    banner.classList.add('remote-active');
    setTimeout(()=>banner.classList.remove('remote-active'),1200);
  }
  await api('/mesh/requestState', { nodeId });
  await api('/mesh/requestKeys', { nodeId });
  // Wait a bit then reload mesh state to pick new state
  setTimeout(loadMeshState, 1000);
  setTimeout(()=>loadMeshKeys(nodeId), 1200);
  setTimeout(loadMeshState, 3000);
  setTimeout(()=>loadMeshKeys(nodeId), 3200);
}

function closeMeshModal(){ closeModal('meshModal'); }

function openModal(id){
  const el = $(id);
  if (!el) return;
  el.classList.remove('hidden');
  if (id === 'gasModal' && typeof loadGas === 'function') loadGas();
  if (id === 'dhtModal' && typeof loadDht === 'function') loadDht();
  if (id === 'nodesModal'){ console.log('[WEB] nodesModal open -> fetch mesh state'); loadMeshState && loadMeshState(); }
  if (id === 'smsModal') loadSmsLog && loadSmsLog();
  if (id === 'inboxModal') loadInboxLog && loadInboxLog();
  if (id === 'memModal') loadMem();
  if (id === 'wifiModal') populateMeshSettings(window.keys || []);
  if (id === 'helpModal') renderHelp();
}

function closeModal(id){
  const el = $(id);
  if (el) el.classList.add('hidden');
}

window.addEventListener('load', tryResumeSession);
</script>

</body>
</html>
)rawliteral";
