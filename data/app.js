let adminToken = localStorage.getItem("admin_token") || "";
let alarmsCache = [];
let filesCache = [];

function buildHeaders(hasBody) {
  const h = {};
  if (hasBody) h["Content-Type"] = "application/json";
  if (adminToken) h["X-Admin-Token"] = adminToken;
  return h;
}

async function apiJson(method, path, body) {
  const res = await fetch(path, {
    method,
    headers: buildHeaders(body !== undefined),
    body: body !== undefined ? JSON.stringify(body) : undefined
  });
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = text; }
  if (!res.ok) throw new Error((data && data.error) ? data.error : (text || res.statusText));
  return data;
}

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text ?? "";
}

function setInputValue(id, value) {
  const el = document.getElementById(id);
  if (el) el.value = value ?? "";
}

function setPreText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text ?? "";
}

function escapeHtml(s) {
  return String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#039;" }[c]));
}

function fmtTime(hh, mm) {
  return `${String(hh).padStart(2, "0")}:${String(mm).padStart(2, "0")}`;
}

function fmtDays(mask, onceDate) {
  if (onceDate) return `en gång ${onceDate}`;
  if (!mask) return "ingen dag";
  const days = ["Mån", "Tis", "Ons", "Tor", "Fre", "Lör", "Sön"];
  const arr = [];
  for (let i = 0; i < 7; i++) if (mask & (1 << i)) arr.push(days[i]);
  return arr.join(", ");
}

function fmtNext(nextUnix) {
  if (!nextUnix) return "-";
  const d = new Date(nextUnix * 1000);
  return d.toLocaleString();
}

async function loadStatus() {
  try {
    const st = await apiJson("GET", "/api/status");
    setText("statusLine", st.wifi_connected ? "Online" : "AP-läge");
    setText("devId", st.device_id || "-");
    setText("nowIso", st.ts_iso || "-");
    setText("ntp", st.ntp_synced ? "synkad" : (st.time_valid ? "tid ok" : "ogiltig tid"));
    setText("ip", st.ip || "-");

    if (st.littlefs) {
      const used = (st.littlefs.used / (1024 * 1024)).toFixed(2);
      const total = (st.littlefs.total / (1024 * 1024)).toFixed(2);
      setText("fsInfo", `LittleFS ${used}/${total} MB`);
    }
  } catch (e) {
    setText("statusLine", "Kunde inte läsa status");
  }
}

function renderAlarmList(alarms) {
  alarmsCache = alarms || [];
  const list = document.getElementById("alarmList");
  list.innerHTML = "";
  if (!alarmsCache.length) {
    list.innerHTML = "<div class=\"small\">Inga alarm</div>";
    return;
  }

  alarmsCache.forEach(a => {
    const item = document.createElement("div");
    item.className = "list-item";
    item.innerHTML = `
      <div class="row">
        <div>
          <div class="h">${escapeHtml(a.label || "Alarm")} ${a.enabled ? "" : "(av)"}</div>
          <div class="small">${fmtTime(a.hour, a.minute)} • ${fmtDays(a.days_bitmask, a.once_date)} • nästa: ${fmtNext(a.next_fire_unix)}</div>
        </div>
        <div class="actions">
          <button class="btn" data-act="edit" data-id="${a.id}">Redigera</button>
          <button class="btn secondary" data-act="toggle" data-id="${a.id}">${a.enabled ? "Stäng av" : "Aktivera"}</button>
          <button class="btn secondary" data-act="fire" data-id="${a.id}">Ring nu</button>
          <button class="btn danger" data-act="delete" data-id="${a.id}">Radera</button>
        </div>
      </div>`;
    list.appendChild(item);
  });
}

async function loadAlarms() {
  try {
    const alarms = await apiJson("GET", "/api/alarms");
    renderAlarmList(alarms);
  } catch (e) {
    setText("alarmList", `Fel: ${e.message || e}`);
  }
}

function fillAlarmDialog(a) {
  setInputValue("alarmId", a.id || "");
  setInputValue("aLabel", a.label || "");
  setInputValue("aTime", fmtTime(a.hour ?? 7, a.minute ?? 30));
  setInputValue("aOnce", a.once_date || "");
  setInputValue("aDays", a.days_bitmask ?? 0);
  setInputValue("aSnooze", a.snooze_minutes ?? 5);
  setInputValue("aGpio", a.gpio_pin ?? 0);
  setInputValue("aLong", a.long_press_ms ?? 0);
  setInputValue("aVol", a.volume ?? 80);
  setInputValue("aInToken", a.inbound_webhook_token || "");

  const as = a.audio_source || {};
  setInputValue("aAudioType", as.type || "local");
  setInputValue("aLocalPath", as.local_path || "/audio/default.wav");
  setInputValue("aUrl", as.url || "");
  setInputValue("aFallback", as.fallback_local_path || "/audio/default.wav");

  const wh = a.outbound_webhooks || {};
  setInputValue("wSet", wh.on_set_url || "");
  setInputValue("wFire", wh.on_fire_url || "");
  setInputValue("wSnooze", wh.on_snooze_url || "");
  setInputValue("wDismiss", wh.on_dismiss_url || "");
}

function readAlarmDialog() {
  const id = document.getElementById("alarmId").value.trim();
  const timeStr = (document.getElementById("aTime").value || "07:30").trim();
  const [hhStr, mmStr] = timeStr.split(":");
  const hh = parseInt(hhStr, 10) || 0;
  const mm = parseInt(mmStr, 10) || 0;

  return {
    id,
    payload: {
      label: document.getElementById("aLabel").value.trim(),
      hour: hh,
      minute: mm,
      days_bitmask: parseInt(document.getElementById("aDays").value || "0", 10),
      once_date: document.getElementById("aOnce").value.trim(),
      snooze_minutes: parseInt(document.getElementById("aSnooze").value || "0", 10),
      gpio_pin: parseInt(document.getElementById("aGpio").value || "0", 10),
      long_press_ms: parseInt(document.getElementById("aLong").value || "0", 10),
      volume: parseInt(document.getElementById("aVol").value || "80", 10),
      inbound_webhook_token: document.getElementById("aInToken").value.trim(),
      audio_source: {
        type: document.getElementById("aAudioType").value,
        local_path: document.getElementById("aLocalPath").value.trim(),
        url: document.getElementById("aUrl").value.trim(),
        fallback_local_path: document.getElementById("aFallback").value.trim()
      },
      outbound_webhooks: {
        on_set_url: document.getElementById("wSet").value.trim(),
        on_fire_url: document.getElementById("wFire").value.trim(),
        on_snooze_url: document.getElementById("wSnooze").value.trim(),
        on_dismiss_url: document.getElementById("wDismiss").value.trim()
      }
    }
  };
}

function populateAudioSelects(selectedLocal, selectedFallback) {
  const opts = ["/audio/default.wav", ...filesCache.map(f => f.path || f.name).filter(Boolean)];
  const normalized = opts.map(p => p.startsWith("/audio/") ? p : `/audio/${p.replace(/^\/+/, "")}`);
  const sel1 = document.getElementById("aLocalPath");
  const sel2 = document.getElementById("aFallback");
  [sel1, sel2].forEach((sel, idx) => {
    if (!sel) return;
    const wanted = idx === 0 ? selectedLocal : selectedFallback;
    sel.innerHTML = "";
    normalized.forEach(p => {
      const opt = document.createElement("option");
      opt.value = p;
      opt.textContent = p;
      sel.appendChild(opt);
    });
    if (wanted && normalized.includes(wanted)) sel.value = wanted;
    else sel.value = "/audio/default.wav";
  });
}

function openAlarmDialog(alarm) {
  fillAlarmDialog(alarm);
  const as = alarm.audio_source || {};
  populateAudioSelects(as.local_path || "/audio/default.wav", as.fallback_local_path || "/audio/default.wav");
  setText("dlgMsg", "");
  const dlg = document.getElementById("dlgAlarm");
  if (dlg && !dlg.open) dlg.showModal();
}

function closeAlarmDialog() {
  const dlg = document.getElementById("dlgAlarm");
  if (dlg && dlg.open) dlg.close();
}

async function createAlarm() {
  try {
    const res = await apiJson("POST", "/api/alarms", { label: "Nytt alarm", enabled: false, hour: 7, minute: 30, days_bitmask: 0 });
    let created = null;
    try { created = await apiJson("GET", `/api/alarms/${res.id}`); } catch {}
    await loadAlarms();
    openAlarmDialog(created || { id: res.id, label: "Nytt alarm", hour: 7, minute: 30, days_bitmask: 0, volume: 80, audio_source: { type: "local", local_path: "/audio/default.wav", fallback_local_path: "/audio/default.wav", url: "" } });
  } catch (e) {
    alert(`Fel vid skapande: ${e.message || e}`);
  }
}

async function saveAlarm() {
  const { id, payload } = readAlarmDialog();
  // Normalisera ljudvägar när typ = local
  if (payload.audio_source.type === "local") {
    const fix = (p) => p && p.length ? (p.startsWith("/audio/") ? p : `/audio/${p.replace(/^\/+/, "")}`) : p;
    payload.audio_source.local_path = fix(payload.audio_source.local_path);
    payload.audio_source.fallback_local_path = fix(payload.audio_source.fallback_local_path);
  }
  try {
    if (!id) {
      const res = await apiJson("POST", "/api/alarms", payload);
      setInputValue("alarmId", res.id || "");
      setText("dlgMsg", "Skapat");
    } else {
      await apiJson("PUT", `/api/alarms/${id}`, payload);
      setText("dlgMsg", "Sparat");
    }
    await loadAlarms();
    await loadStatus();
  } catch (e) {
    setText("dlgMsg", `Fel: ${e.message || e}`);
  }
}

async function toggleAlarm(id) {
  const a = alarmsCache.find(x => String(x.id) === String(id));
  if (!a) return;
  await apiJson("POST", `/api/alarms/${id}/${a.enabled ? "disable" : "enable"}`);
  await loadAlarms();
  await loadStatus();
}

async function deleteAlarm(id) {
  if (!confirm("Radera alarmet?")) return;
  await apiJson("DELETE", `/api/alarms/${id}`);
  await loadAlarms();
  await loadStatus();
  closeAlarmDialog();
}

async function fireAlarm(id) {
  await apiJson("POST", `/api/alarms/${id}/fire`);
  await loadStatus();
}

async function testAlarmAudio(id) {
  const res = await apiJson("POST", `/api/alarms/${id}/test_audio`);
  setText("dlgMsg", `Test: ok=${res.ok} ${res.last_audio_error || ""}`);
  await loadStatus();
}

async function loadFiles() {
  try {
    filesCache = await apiJson("GET", "/api/files");
  } catch (e) {
    filesCache = [];
    setText("fileList", `Fel: ${e.message || e}`);
    return;
  }
  populateAudioSelects();
  const list = document.getElementById("fileList");
  list.innerHTML = "";
  if (!filesCache.length) {
    list.innerHTML = "<div class=\"small\">Inga filer</div>";
    return;
  }
  filesCache.forEach(f => {
    const row = document.createElement("div");
    row.className = "list-item";
    row.innerHTML = `
      <div class="row">
        <div>
          <div>${escapeHtml(f.name || "")}</div>
          <div class="small">${f.size} bytes • ${escapeHtml(f.path || "")}</div>
        </div>
        <div class="actions">
          <button class="btn danger" data-path="${escapeHtml(f.path || "")}">Radera</button>
        </div>
      </div>`;
    list.appendChild(row);
  });
}

async function deleteFile(path) {
  await fetch(`/api/files?path=${encodeURIComponent(path)}`, {
    method: "DELETE",
    headers: buildHeaders(false)
  }).then(async res => {
    const t = await res.text();
    if (!res.ok) throw new Error(t || res.statusText);
  });
}

async function uploadFile() {
  const inp = document.getElementById("filePick");
  if (!inp.files || !inp.files[0]) { setText("fsInfo", "Välj en fil"); return; }
  const file = inp.files[0];
  if (file.size > 2 * 1024 * 1024) { setText("fsInfo", "Max 2 MB"); return; }
  const fd = new FormData();
  fd.append("file", file);
  setText("fsInfo", "Laddar upp...");
  const res = await fetch("/api/files/upload", {
    method: "POST",
    headers: adminToken ? { "X-Admin-Token": adminToken } : {},
    body: fd
  });
  const txt = await res.text();
  if (!res.ok) { setText("fsInfo", `Fel: ${txt}`); return; }
  setText("fsInfo", "Klart");
  inp.value = "";
  await loadFiles();
  await loadStatus();
}

async function loadLogs() {
  try {
    const logs = await apiJson("GET", "/api/logs");
    setPreText("logBox", (logs || []).join("\n"));
  } catch (e) {
    setPreText("logBox", `Fel: ${e.message || e}`);
  }
}

async function exportConfig() {
  try {
    const cfg = await apiJson("GET", "/api/config/export");
    document.getElementById("exportBox").value = JSON.stringify(cfg, null, 2);
    setText("fsInfo", "Export klar");
  } catch (e) {
    alert(`Fel: ${e.message || e}`);
  }
}

async function importConfig() {
  const txt = document.getElementById("exportBox").value.trim();
  if (!txt) { alert("Klistra in JSON först"); return; }
  try {
    const cfg = JSON.parse(txt);
    await apiJson("POST", "/api/config/import", cfg);
    alert("Import klar");
    await loadAlarms();
    await loadStatus();
  } catch (e) {
    alert(`Fel vid import: ${e.message || e}`);
  }
}

async function restartDevice() {
  try {
    await apiJson("POST", "/api/system/restart");
    alert("Restart skickad");
  } catch (e) {
    alert(`Fel: ${e.message || e}`);
  }
}

function bindUI() {
  document.getElementById("adminToken").value = adminToken;

  document.getElementById("btnSaveToken").onclick = () => {
    adminToken = document.getElementById("adminToken").value.trim();
    localStorage.setItem("admin_token", adminToken);
    alert("Token sparad i webbläsaren");
  };
  document.getElementById("btnClearToken").onclick = () => {
    adminToken = "";
    localStorage.removeItem("admin_token");
    document.getElementById("adminToken").value = "";
  };

  document.getElementById("btnRefresh").onclick = async () => {
    await loadStatus();
    await loadAlarms();
    await loadFiles();
    await loadLogs();
  };
  document.getElementById("btnWifi").onclick = () => { window.location.href = "/wifi"; };
  document.getElementById("btnNewAlarm").onclick = createAlarm;

  const list = document.getElementById("alarmList");
  list.onclick = async (e) => {
    const btn = e.target.closest("button");
    if (!btn) return;
    const id = btn.getAttribute("data-id");
    const act = btn.getAttribute("data-act");
    try {
      if (act === "edit") {
        let a = alarmsCache.find(x => String(x.id) === String(id));
        if (!a) a = await apiJson("GET", `/api/alarms/${id}`);
        if (!a.id) a.id = id;
        openAlarmDialog(a);
      }
      if (act === "toggle") await toggleAlarm(id);
      if (act === "fire") await fireAlarm(id);
      if (act === "delete") await deleteAlarm(id);
    } catch (err) {
      alert(err.message || err);
    }
  };

  document.getElementById("btnUpload").onclick = () => { uploadFile().catch(e => setText("fsInfo", e.message || e)); };
  document.getElementById("fileList").onclick = async (e) => {
    const btn = e.target.closest("button[data-path]");
    if (!btn) return;
    const path = btn.getAttribute("data-path");
    if (!confirm(`Radera ${path}?`)) return;
    try {
      await deleteFile(path);
      await loadFiles();
      await loadStatus();
    } catch (err) {
      alert(err.message || err);
    }
  };

  document.getElementById("btnExport").onclick = exportConfig;
  document.getElementById("btnImport").onclick = importConfig;
  document.getElementById("btnRestart").onclick = restartDevice;

  const dlg = document.getElementById("dlgAlarm");
  if (dlg) {
    dlg.querySelector("#btnCancel").onclick = (e) => { e.preventDefault(); closeAlarmDialog(); };
    dlg.querySelector("#btnSave").onclick = async (e) => { e.preventDefault(); await saveAlarm(); };
    dlg.querySelector("#btnTestAudio").onclick = async (e) => {
      e.preventDefault();
      const id = document.getElementById("alarmId").value.trim();
      if (!id) { setText("dlgMsg", "Inget alarm valt"); return; }
      try {
        await testAlarmAudio(id);
      } catch (err) {
        setText("dlgMsg", `Fel: ${err.message || err}`);
      }
    };
    dlg.querySelector("#btnFireNow").onclick = async (e) => {
      e.preventDefault();
      const id = document.getElementById("alarmId").value.trim();
      if (id) await fireAlarm(id);
    };
  }
}

async function boot() {
  bindUI();
  await loadStatus();
  await loadAlarms();
  await loadFiles();
  await loadLogs();
  setInterval(loadStatus, 5000);
  setInterval(loadLogs, 5000);
}

boot();
