(function () {
  // Run only after DOM and Leaflet are ready
  function ready(fn) {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", fn, { once: true });
    } else {
      fn();
    }
  }

  ready(async function main() {
    if (typeof L === "undefined") {
      console.error("Leaflet not loaded");
      return;
    }

    // ---- 1) Get device/home info first (for initial center) ----
    let homeLat = 0, homeLon = 0, locToken = "";

    try {
      const cfg = await fetch("/config.json", { cache: "no-store" }).then(r => r.json());
      if (cfg && cfg.home) {
        homeLat = Number(cfg.home.lat) || 0;
        homeLon = Number(cfg.home.lon) || 0;
      }
      if (cfg && cfg.loc_token) {
        locToken = String(cfg.loc_token || "");
      }
      // Render device box
      const di = document.getElementById("deviceinfo");
      if (di && cfg && cfg.wifi) {
        di.innerHTML = `
          <div><b>Wi-Fi:</b> ${cfg.wifi.ssid || "(none)"} (${cfg.wifi.ip || "-"})</div>
          <div><b>Home:</b> ${homeLat.toFixed(4)}, ${homeLon.toFixed(4)}</div>
        `;
      }
    } catch (e) {
      console.warn("config.json failed:", e);
    }

    // ---- 2) Map init ----
    const mapEl = document.getElementById("map");
    if (!mapEl) {
      console.error("#map element missing");
      return;
    }

    const map = L.map("map").setView([homeLat, homeLon], 4);

    // OSM tiles
    L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
      maxZoom: 18,
      attribution: "&copy; OpenStreetMap contributors"
    }).addTo(map);

    // Home marker (green) — NOW DRAGGABLE to set new home
    const homeIcon = L.icon({
      iconUrl: "https://raw.githubusercontent.com/pointhi/leaflet-color-markers/master/img/marker-icon-green.png",
      iconSize: [25, 41],
      iconAnchor: [12, 41]
    });
    const homeMarker = L.marker([homeLat, homeLon], {
      title: "Home",
      icon: homeIcon,
      draggable: true
    }).addTo(map);

    homeMarker.on("dragend", async () => {
      const ll = homeMarker.getLatLng();
      const newLat = Number(ll.lat.toFixed(6));
      const newLon = Number(ll.lng.toFixed(6));

      try {
        const headers = { "Content-Type": "application/json" };
        if (locToken) headers["Authorization"] = "Bearer " + locToken;

        const r = await fetch("/loc", {
          method: "POST",
          headers,
          body: JSON.stringify({ lat: newLat, lon: newLon })
        });
        if (!r.ok) throw new Error("loc failed " + r.status);

        homeLat = newLat;
        homeLon = newLon;

        // Update device box + immediately update link line next poll
        const di = document.getElementById("deviceinfo");
        if (di) {
          di.innerHTML = `
            <div><b>Wi-Fi:</b> (refreshing)</div>
            <div><b>Home:</b> ${homeLat.toFixed(4)}, ${homeLon.toFixed(4)}</div>
          `;
        }
      } catch (e) {
        alert("Failed to save new Home: " + e.message);
        homeMarker.setLatLng([homeLat, homeLon]); // revert
      }
    });

    // ISS marker + track + link line
    let issMarker = null;
    const track = L.polyline([], { color: "orange", weight: 2 }).addTo(map);
    let linkLine = null;

    function haversineKm(a, b) {
      const R = 6371;
      const dLat = (b.lat - a.lat) * Math.PI / 180;
      const dLon = (b.lon - a.lon) * Math.PI / 180;
      const la1 = a.lat * Math.PI / 180;
      const la2 = b.lat * Math.PI / 180;
      const sinDLat = Math.sin(dLat / 2), sinDLon = Math.sin(dLon / 2);
      const x = sinDLat * sinDLat + Math.cos(la1) * Math.cos(la2) * sinDLon * sinDLon;
      return R * 2 * Math.atan2(Math.sqrt(x), Math.sqrt(1 - x));
    }

    // ---- 3) Load any persisted track once (served by firmware) ----
    async function loadTrackOnce() {
      try {
        const r = await fetch("/track.json", { cache: "no-store" });
        if (!r.ok) return; // if endpoint not present yet, skip silently
        const j = await r.json();
        if (!j || !Array.isArray(j.points)) return;

        const pts = j.points
          .filter(p => typeof p.lat === "number" && typeof p.lon === "number")
          .map(p => [p.lat, p.lon]);

        if (pts.length) {
          track.setLatLngs(pts);
          // Keep view reasonable: only pan if home is (0,0)
          if (!homeLat && !homeLon) {
            map.fitBounds(track.getBounds(), { padding: [20, 20] });
          }
        }
      } catch (e) {
        // ignore — device may not have implemented persistence yet
      }
    }

    // ---- 4) Regular live polling ----
    async function pollIss() {
      try {
        const r = await fetch("/iss.json", { cache: "no-store" });
        if (!r.ok) return;
        const j = await r.json();
        if (!j.haveFix || !j.iss) return;

        const p = { lat: Number(j.iss.lat), lon: Number(j.iss.lon) };

        // Marker
        if (!issMarker) {
          issMarker = L.circleMarker([p.lat, p.lon], {
            radius: 6, color: "#c00", fillColor: "#f33", fillOpacity: 0.9
          }).addTo(map).bindPopup("ISS");
        } else {
          issMarker.setLatLng([p.lat, p.lon]);
        }

        // Track (append on the client; firmware will persist/serve /track.json)
        const cur = track.getLatLngs();
        cur.push([p.lat, p.lon]);
        if (cur.length > 720) cur.shift(); // UI-side cap ~1h at 5s cadence
        track.setLatLngs(cur);

        // Link line
        if (linkLine) map.removeLayer(linkLine);
        linkLine = L.polyline([[homeLat, homeLon], [p.lat, p.lon]], {
          color: "green", weight: 1, dashArray: "4,6"
        }).addTo(map);

        // Telemetry
        const dist = haversineKm({ lat: homeLat, lon: homeLon }, p).toFixed(1);
        const vel = (j.iss.vel !== undefined && j.iss.vel === j.iss.vel) ? Number(j.iss.vel).toFixed(0) : "---";
        const dir = (j.iss.dir !== undefined) ? j.iss.dir : "---";

        const tel = document.getElementById("telemetry");
        if (tel) {
          tel.innerHTML = `
            <table class="table table-sm mb-0">
              <tr><th>ISS Lat</th><td>${p.lat.toFixed(4)}</td></tr>
              <tr><th>ISS Lon</th><td>${p.lon.toFixed(4)}</td></tr>
              <tr><th>Distance</th><td>${dist} km</td></tr>
              <tr><th>Velocity</th><td>${vel} km/h</td></tr>
              <tr><th>Direction</th><td>${dir}</td></tr>
              <tr><th>Home</th><td>${homeLat.toFixed(4)}, ${homeLon.toFixed(4)}</td></tr>
            </table>`;
        }
      } catch {
        // ignore transient fetch errors
      }
    }

    // ---- 5) Buttons ----
    const btnHome = document.getElementById("centerHome");
    if (btnHome) btnHome.onclick = () => map.setView([homeLat, homeLon], 4);

    const btnIss = document.getElementById("centerIss");
    if (btnIss) btnIss.onclick = () => { if (issMarker) map.setView(issMarker.getLatLng(), 4); };

    // ---- 6) Go ----
    await loadTrackOnce();  // draw persisted path (if firmware serves it)
    await pollIss();        // first immediate poll
    setInterval(pollIss, 5000);

    // Ensure tiles render if container resized
    setTimeout(() => map.invalidateSize(), 200);
  });
})();

