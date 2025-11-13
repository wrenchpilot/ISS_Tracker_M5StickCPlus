(function(){
  async function getJSON(url){
    const r = await fetch(url, {cache:'no-store'});
    if(!r.ok) throw new Error(url+" http "+r.status);
    return await r.json();
  }

  function fillDeviceBox(cfg){
    const devSsid = document.getElementById('devSsid');
    const devIp   = document.getElementById('devIp');
    const devHome = document.getElementById('devHome');
    if (devSsid) devSsid.textContent = cfg?.wifi?.ssid ?? '—';
    if (devIp)   devIp.textContent   = cfg?.wifi?.ip   ?? '—';
    if (devHome) devHome.textContent = (cfg?.home?.lat?.toFixed && cfg?.home?.lon?.toFixed)
      ? `${cfg.home.lat.toFixed(4)}, ${cfg.home.lon.toFixed(4)}`
      : '—';
  }

  function populateSSIDs(nets){
    const sel = document.getElementById('ssidSelect');
    if (!sel) return;
    sel.innerHTML = '';
    if (!nets || !nets.length){
      const opt = document.createElement('option');
      opt.disabled = true; opt.textContent = '(no networks found)';
      sel.appendChild(opt);
      return;
    }
    nets.sort((a,b)=>b.rssi-a.rssi);
    for (const n of nets){
      const opt = document.createElement('option');
      const lock = n.locked ? '🔒' : '🔓';
      opt.value = n.ssid;
      opt.title = `${n.ssid} (${n.rssi} dBm)`;
      opt.textContent = `${lock} ${n.ssid}  (${n.rssi} dBm)`;
      sel.appendChild(opt);
    }
  }

  async function scanOnce(){
    try {
      const j = await getJSON('/scan.json');
      populateSSIDs(j?.nets || []);
    } catch(e) {
      // leave previous list
    }
  }

  async function main(){
    const rescanBtn = document.getElementById('rescanBtn');
    const ssidSel   = document.getElementById('ssidSelect');
    const ssidInput = document.getElementById('ssidInput');
    const passInput = document.getElementById('passInput');
    const forgetBtn = document.getElementById('forgetBtn');
    const forgetForm= document.getElementById('forgetForm');

    // device info
    try { fillDeviceBox(await getJSON('/config.json')); } catch(e){}

    // initial scan
    scanOnce();

    if (rescanBtn) rescanBtn.onclick = () => scanOnce();

    if (ssidSel && ssidInput){
      ssidSel.addEventListener('change', () => {
        const v = ssidSel.value || '';
        ssidInput.value = v;
        if (passInput) passInput.focus();
      });
      // Also set on double click for quick fill
      ssidSel.addEventListener('dblclick', () => {
        const v = ssidSel.value || '';
        ssidInput.value = v;
      });
    }

    if (forgetBtn && forgetForm){
      forgetBtn.onclick = () => { forgetForm.submit(); };
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', main);
  } else {
    main();
  }
})();

