(() => {
  // WhereTheISS.at (km units)
  const WISS_NOW = 'https://api.wheretheiss.at/v1/satellites/25544?units=kilometers';
  const WISS_POS = 'https://api.wheretheiss.at/v1/satellites/25544/positions'; // ?timestamps=,&units=kilometers

  // DOM helpers
  const $ = (id) => document.getElementById(id);
  const on = (el, ev, fn) => el && el.addEventListener(ev, fn);

  // Map/layers
  let map, base, terminator;
  let didInitialFit = false; // run “Fit All” once when content exists

  // Live ISS marker (circle)
  const issIcon = L.circleMarker([0, 0], { radius: 6, color: '#c00', fillColor: '#f33', fillOpacity: 0.9 });

  // Past track (persisted + live extension)
  const track = L.polyline([], { color: 'orange', weight: 2 });

  // 1-hour prediction (blue dotted)
  const predict = L.polyline([], { color: '#2b6cb0', weight: 2, dashArray: '6,6' });

  // Home marker (draggable)
  let homeLat = 0, homeLon = 0;
  let homeMarker = null;
  const homeIcon = L.icon({
    iconUrl: 'https://raw.githubusercontent.com/pointhi/leaflet-color-markers/master/img/marker-icon-green.png',
    iconSize: [25, 41],
    iconAnchor: [12, 41]
  });

  // Sun marker (from solar_lat / solar_lon)
  const sunIcon = L.circleMarker([0, 0], { radius: 6, color: '#ffd400', fillColor: '#ffd400', fillOpacity: 1.0 });

  // Link line Home ↔ ISS
  let linkLine = null;

  // Last ISS sample (raw API object)
  let lastIss = null;

  // Last 8-point direction label for velocity row
  let lastDir8 = '—';

  // Utilities
  function qs(obj) {
    return Object.entries(obj).map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`).join('&');
  }

  // Great-circle distance (km)
  function haversineKm(a, b) {
    const R = 6371;
    const dLat = (b.lat - a.lat) * Math.PI / 180;
    const dLon = (b.lon - a.lon) * Math.PI / 180;
    const la1 = a.lat * Math.PI / 180, la2 = b.lat * Math.PI / 180;
    const x = Math.sin(dLat / 2) ** 2 + Math.cos(la1) * Math.cos(la2) * Math.sin(dLon / 2) ** 2;
    return R * 2 * Math.atan2(Math.sqrt(x), Math.sqrt(1 - x));
  }

  // Initial bearing (deg) from point A(lat1,lon1) to B(lat2,lon2)
  function initialBearingDeg(lat1, lon1, lat2, lon2) {
    const φ1 = lat1 * Math.PI / 180;
    const φ2 = lat2 * Math.PI / 180;
    // normalize longitudes to [-180,180] before diff to avoid anti-meridian explosions
    const norm = (x) => ((x + 540) % 360) - 180;
    const λ1 = norm(lon1) * Math.PI / 180;
    const λ2 = norm(lon2) * Math.PI / 180;
    const y = Math.sin(λ2 - λ1) * Math.cos(φ2);
    const x = Math.cos(φ1) * Math.sin(φ2) - Math.sin(φ1) * Math.cos(φ2) * Math.cos(λ2 - λ1);
    let θ = Math.atan2(y, x) * 180 / Math.PI; // [-180,180]
    if (θ < 0) θ += 360;
    return θ; // [0,360)
  }

  function bearingTo8(deg) {
    // 8-wind compass, 45° sectors centered on the cardinals
    const labels = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
    const idx = Math.round(deg / 45) % 8;
    return labels[idx];
  }

  function fmtAge(sec) {
    if (!Number.isFinite(sec) || sec < 0) return '—';
    if (sec < 90) return `${Math.round(sec)} s`;
    const m = Math.floor(sec / 60), s = Math.round(sec % 60);
    return `${m}m ${s}s`;
  }

  // --- Antimeridian helpers: split polylines at ±180 so Leaflet doesn’t draw a long straight wrap ---
  function splitAtDateline(latlngs) {
    if (!latlngs || latlngs.length < 2) return latlngs;
    const out = [];
    let seg = [];
    let prev = null;

    const normLng = (lng) => {
      // normalize to [-180, 180]
      let x = lng;
      while (x > 180) x -= 360;
      while (x < -180) x += 360;
      return x;
    };

    for (const ll of latlngs) {
      const cur = L.latLng(ll.lat ?? ll[0], normLng(ll.lng ?? ll.lon ?? ll[1]), ll.alt);
      if (!prev) {
        seg.push(cur);
        prev = cur;
        continue;
      }
      const dLon = Math.abs(cur.lng - prev.lng);
      if (dLon > 180) {
        // break the line to avoid a long cross-map segment
        out.push(seg);
        seg = [cur];
      } else {
        seg.push(cur);
      }
      prev = cur;
    }
    if (seg.length) out.push(seg);
    // Leaflet accepts an array of arrays to make multiple segments
    return out.length === 1 ? out[0] : out;
  }

  // Bounds composed from everything we show
  function layerBounds() {
    const layers = [];
    if (homeMarker) layers.push(homeMarker.getLatLng());
    if (issIcon && issIcon.getLatLng) layers.push(issIcon.getLatLng());
    const t = track.getLatLngs();
    if (t && (Array.isArray(t[0]) ? t.flat().length : t.length)) {
      (Array.isArray(t[0]) ? t.flat() : t).forEach(p => layers.push(p));
    }
    const p = predict.getLatLngs();
    if (p && (Array.isArray(p[0]) ? p.flat().length : p.length)) {
      (Array.isArray(p[0]) ? p.flat() : p).forEach(p2 => layers.push(p2));
    }
    if (sunIcon && sunIcon.getLatLng) layers.push(sunIcon.getLatLng());
    if (!layers.length) return null;
    return L.latLngBounds(layers);
  }

  // Fit All: prefer actual layer bounds; otherwise fit entire world
  function fitAll(pad = [30, 30]) {
    const b = layerBounds();
    if (b) {
      map.fitBounds(b, { paddingTopLeft: pad, paddingBottomRight: pad, animate: false });
    } else {
      map.fitWorld({ animate: false });
    }
  }
  function fitAllOnce() {
    if (!didInitialFit) {
      didInitialFit = true;
      fitAll();
    }
  }

  function addOrRefreshTerminator() {
    if (!L.terminator) return; // plugin missing → skip
    const t = L.terminator({
      resolution: 4,
      fillOpacity: 0.28,
      color: '#000000',
      fillColor: '#000000',
      stroke: false
    });
    if (terminator) map.removeLayer(terminator);
    terminator = t.addTo(map);
  }

  async function loadDeviceConfig() {
    try {
      const r = await fetch('/config.json', { cache: 'no-store' });
      if (!r.ok) return;
      const j = await r.json();
      if (j && j.home) {
        homeLat = Number(j.home.lat) || 0;
        homeLon = Number(j.home.lon) || 0;
      }
    } catch (_) { /* ignore */ }
  }

  async function loadPersistedTrack() {
    // Expect array of {lat, lon, ts} for the last hour (device provides it)
    try {
      const r = await fetch('/track.json', { cache: 'no-store' });
      if (!r.ok) return;
      const j = await r.json();
      if (Array.isArray(j)) {
        const pts = j
          .filter(p => Number.isFinite(p.lat) && Number.isFinite(p.lon))
          .map(p => L.latLng(Number(p.lat), Number(p.lon)));
        const split = splitAtDateline(pts);
        track.setLatLngs(split);
      }
    } catch (_) { /* ignore */ }
  }

  async function fetchIssNow() {
    const r = await fetch(WISS_NOW, { cache: 'no-store' });
    if (!r.ok) throw new Error('iss_now http ' + r.status);
    return r.json();
  }

  async function fetchPredictions(startTs) {
    // 1 hour ahead, every 2 minutes (31 samples)
    const step = 120; // seconds
    const count = 31;
    const arr = Array.from({ length: count }, (_, i) => startTs + i * step);
    const url = `${WISS_POS}?${qs({ timestamps: arr.join(','), units: 'kilometers' })}`;
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error('iss_positions http ' + r.status);
    return r.json();
  }

  function updateTelemetry(data) {
    const el = $('telemetry');
    if (!el) return;

    const lat = Number(data.latitude);
    const lon = Number(data.longitude);
    const alt = Number(data.altitude);
    const vel = Number(data.velocity);
    const vis = data.visibility || '—';
    const fpt = Number(data.footprint);
    const ts = Number(data.timestamp);
    const sLat = Number(data.solar_lat);
    const sLon = Number(data.solar_lon);

    let dist = '—';
    if (homeMarker && Number.isFinite(lat) && Number.isFinite(lon)) {
      dist = haversineKm(
        { lat: homeMarker.getLatLng().lat, lon: homeMarker.getLatLng().lng },
        { lat, lon }
      ).toFixed(1);
    }

    let age = '—';
    if (Number.isFinite(ts)) {
      age = fmtAge(Math.max(0, (Date.now() / 1000) - ts));
    }

    el.innerHTML = [
      "<table class='table table-sm mb-0'>",
      `<tr><th>ISS Lat</th><td>${Number.isFinite(lat) ? lat.toFixed(4) : '—'}</td></tr>`,
      `<tr><th>ISS Lon</th><td>${Number.isFinite(lon) ? lon.toFixed(4) : '—'}</td></tr>`,
      `<tr><th>Distance</th><td>${dist} km</td></tr>`,
      `<tr><th>Velocity</th><td>${Number.isFinite(vel) ? vel.toFixed(0) : '—'} km/h ${lastDir8}</td></tr>`,
      `<tr><th>Height</th><td>${Number.isFinite(alt) ? alt.toFixed(1) : '—'} km</td></tr>`,
      `<tr><th>Visibility</th><td>${vis}</td></tr>`,
      `<tr><th>Footprint</th><td>${Number.isFinite(fpt) ? fpt.toFixed(0) : '—'} km</td></tr>`,
      `<tr><th>Solar Lat</th><td>${Number.isFinite(sLat) ? sLat.toFixed(2) : '—'}</td></tr>`,
      `<tr><th>Solar Lon</th><td>${Number.isFinite(sLon) ? sLon.toFixed(2) : '—'}</td></tr>`,
      homeMarker
        ? `<tr><th>Home</th><td>${homeMarker.getLatLng().lat.toFixed(4)}, ${homeMarker.getLatLng().lng.toFixed(4)}</td></tr>`
        : '',
      '</table>'
    ].join('');
  }

  function updateLines(lat, lon) {
    // Home ↔ ISS
    if (linkLine) map.removeLayer(linkLine);
    if (homeMarker) {
      linkLine = L.polyline([[homeMarker.getLatLng().lat, homeMarker.getLatLng().lng], [lat, lon]], {
        color: 'green',
        weight: 1,
        dashArray: '4,6'
      }).addTo(map);
    }
  }

  function updateSun(lat, lon) {
    if (!Number.isFinite(lat) || !Number.isFinite(lon)) return;
    // Normalize longitude to [-180, 180] so markers appear correctly on a no-wrap map
    let nl = lon;
    while (nl > 180) nl -= 360;
    while (nl < -180) nl += 360;
    sunIcon.setLatLng([lat, nl]);
    if (!sunIcon._map) sunIcon.addTo(map);
  }

  async function refreshPrediction(tsBase) {
    try {
      const preds = await fetchPredictions(tsBase);
      const pts = preds
        .filter(p => Number.isFinite(p.latitude) && Number.isFinite(p.longitude))
        .map(p => L.latLng(p.latitude, p.longitude));
      const split = splitAtDateline(pts);
      predict.setLatLngs(split);
      fitAllOnce();
    } catch (e) {
      console.warn('Prediction error:', e.message || e);
    }
  }

  async function poll() {
    try {
      const data = await fetchIssNow();

      const lat = Number(data.latitude);
      const lon = Number(data.longitude);

      if (Number.isFinite(lat) && Number.isFinite(lon)) {
        // Determine bearing vs previous point (persisted or live)
        const ptsExisting = track.getLatLngs();
        const flat = Array.isArray(ptsExisting[0]) ? ptsExisting.flat() : ptsExisting;
        const prev = flat.length ? flat[flat.length - 1] : null;
        if (prev && Number.isFinite(prev.lat) && Number.isFinite(prev.lng)) {
          const brg = initialBearingDeg(prev.lat, prev.lng, lat, lon);
          lastDir8 = bearingTo8(brg);
        } else {
          lastDir8 = '—';
        }

        // Live marker
        issIcon.setLatLng([lat, lon]);
        if (!issIcon._map) issIcon.addTo(map);

        // Extend live track; keep a sane cap
        const next = flat.slice();
        next.push(L.latLng(lat, lon));
        if (next.length > 2000) next.splice(0, next.length - 2000);
        track.setLatLngs(splitAtDateline(next));

        // Lines + telemetry
        updateLines(lat, lon);
        updateTelemetry(data);

        // Sun & day/night
        updateSun(Number(data.solar_lat), Number(data.solar_lon));
        addOrRefreshTerminator();

        // Predictions: first time or if stale
        if (!predict.getLatLngs().length || (Date.now() / 1000 - Number(data.timestamp)) > 60) {
          refreshPrediction(Number(data.timestamp));
        }

        lastIss = data;
        fitAllOnce(); // ensure default "Fit All" once we have content
      }
    } catch (e) {
      console.warn('Live fetch error:', e.message || e);
    }

    // Check for home location updates (e.g., from external /loc API calls)
    try {
      const r = await fetch('/config.json', { cache: 'no-store' });
      if (r.ok) {
        const j = await r.json();
        if (j && j.home) {
          const newLat = Number(j.home.lat);
          const newLon = Number(j.home.lon);
          if (Number.isFinite(newLat) && Number.isFinite(newLon)) {
            // Check if home location changed
            const currentHome = homeMarker ? homeMarker.getLatLng() : null;
            if (!currentHome || Math.abs(currentHome.lat - newLat) > 0.0001 || Math.abs(currentHome.lng - newLon) > 0.0001) {
              homeLat = newLat;
              homeLon = newLon;
              if (homeMarker) {
                homeMarker.setLatLng([newLat, newLon]);
                // Redraw lines and telemetry with new home position
                if (lastIss && Number.isFinite(lastIss.latitude) && Number.isFinite(lastIss.longitude)) {
                  updateLines(Number(lastIss.latitude), Number(lastIss.longitude));
                  updateTelemetry(lastIss);
                }
              }
            }
          }
        }
      }
    } catch (e) {
      console.warn('Config check error:', e.message || e);
    }
  }

  async function init() {
    await loadDeviceConfig();

    // Map with **constrained world** (no repeating), reasonable minZoom
    map = L.map('map', {
      worldCopyJump: false,
      maxBounds: [[-85, -180], [85, 180]],
      maxBoundsViscosity: 1.0,
      minZoom: 2
    });
    base = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 12,
      noWrap: true,            // <- key: no world wrap tiles
      bounds: [[-85, -180], [85, 180]],
      attribution: '&copy; OpenStreetMap'
    }).addTo(map);

    // Add layers
    track.addTo(map);
    predict.addTo(map);

    // Home marker (draggable)
    const startHome = [Number.isFinite(homeLat) ? homeLat : 0, Number.isFinite(homeLon) ? homeLon : 0];
    homeMarker = L.marker(startHome, { title: 'Home', icon: homeIcon, draggable: true }).addTo(map);
    homeMarker.on('dragend', async () => {
      const p = homeMarker.getLatLng();
      try {
        await fetch('/savehome', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: qs({ lat: p.lat, lon: p.lng })
        });
      } catch (_) { /* ignore */ }
      if (lastIss && Number.isFinite(lastIss.latitude) && Number.isFinite(lastIss.longitude)) {
        updateLines(Number(lastIss.latitude), Number(lastIss.longitude));
        updateTelemetry(lastIss);
      }
      fitAll(); // reflect new “home” in bounds
    });

    // Buttons
    on($('centerHome'), 'click', () => {
      if (homeMarker) map.setView(homeMarker.getLatLng(), 4);
    });
    on($('centerIss'), 'click', () => {
      if (issIcon && issIcon.getLatLng) map.setView(issIcon.getLatLng(), 4);
    });
    on($('fitAll'), 'click', () => fitAll());

    // Legend toggle
    on($('legendToggle'), 'click', () => {
      const panel = $('legendPanel');
      if (panel) panel.classList.toggle('show');
    });

    // Day/Night overlay (plugin URL is included in index.html)
    addOrRefreshTerminator();
    setInterval(addOrRefreshTerminator, 60 * 1000);

    // Device screenshot refresh (every 5 seconds, cache-bust)
    function refreshScreenshot() {
      const img = $('deviceScreen');
      if (img) {
        img.src = `/screen.bmp?t=${Date.now()}`;
      }
    }
    setInterval(refreshScreenshot, 5000);

    // Handle map resize and invalidate size when window resizes
    let resizeTimeout;
    window.addEventListener('resize', () => {
      clearTimeout(resizeTimeout);
      resizeTimeout = setTimeout(() => {
        if (map) {
          map.invalidateSize();
          // Recenter on ISS if available, otherwise fit all
          if (issIcon && issIcon.getLatLng && issIcon.getLatLng().lat !== 0) {
            map.setView(issIcon.getLatLng(), map.getZoom());
          } else {
            fitAll();
          }
        }
      }, 250);
    });

    // Load persisted track first so path is visible before live data
    await loadPersistedTrack();

    // Default: world view if we only have home/track so far
    if (!track.getLatLngs().length) {
      map.fitWorld({ animate: true });
    } else {
      fitAllOnce();
    }

    // First live sample + periodic polling
    await poll();
    
    // After first data load, invalidate map size to handle flexbox layout
    // and refit bounds to ensure proper centering
    setTimeout(() => {
      if (map) {
        map.invalidateSize();
        if (!didInitialFit) {
          fitAll();
          didInitialFit = true;
        }
      }
    }, 100);
    
    setInterval(poll, 5000);
  }

  // Boot once DOM is ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
