const $ = (id) => document.getElementById(id);
try { window.resizeTo(720, 440); } catch (e) {}
const BROWSE_TO = { game: "game_folder", assets: "assets", discs_fe1: "discs_fe1", discs_fe2: "discs_fe2" };
const PATH_IDS = ["game_folder", "assets", "discs_fe1", "discs_fe2"];
const state = { game_folder: "", assets: "", discs_fe1: "", discs_fe2: "", busy: false, logOpen: false };
const peekTimer = {};

function setPath(id, v, peek) {
  state[id] = v || "";
  const el = $(id);
  if (el && el.value !== (v || "")) el.value = v || "";
  if (id === "discs_fe1" || id === "discs_fe2") setPeek(id, peek);
}

function setPeek(id, peek) {
  const note = $(id + "_note");
  if (!note) return;
  if (!peek) {
    note.textContent = "";
    return;
  }
  const bits = [];
  if (peek.fe1) bits.push("FE1 ×" + peek.fe1);
  if (peek.fe2) bits.push("FE2 ×" + peek.fe2);
  if (peek.unknown) bits.push(peek.unknown + " ?");
  const other = id === "discs_fe1" ? "fe2" : "fe1";
  const mine = id === "discs_fe1" ? "fe1" : "fe2";
  if (peek[other] && !peek[mine]) bits.push("→ " + (other === "fe2" ? "Fear Effect 2" : "Fear Effect"));
  else if (peek.fe1 && peek.fe2) bits.push("both, split");
  note.textContent = bits.join(" · ");
}

function setStatus(kind, text) {
  const wrap = $("progress_wrap");
  wrap.hidden = !text;
  $("progress_track").hidden = kind !== "busy" && kind !== "ok";
  const el = $("status");
  el.className = "status " + kind;
  el.textContent = text || "";
  document.body.classList.toggle("error", kind === "err");
  document.body.classList.toggle("ok", kind === "ok");
}

function setProgress(frac) {
  const pct = Math.max(0, Math.min(100, Math.round((frac || 0) * 100)));
  $("progress_bar").style.width = pct + "%";
}

async function api(path, body) {
  const r = await fetch(path, {
    method: body ? "POST" : "GET",
    headers: body ? { "Content-Type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

function showLog(on) {
  state.logOpen = !!on;
  if (on && !$("log").textContent.trim()) $("log").textContent = "(no output yet)";
  $("logbox").hidden = !on;
  $("toggle_log").textContent = on ? "Hide log" : "Log";
}

async function peekPath(id) {
  if (id !== "discs_fe1" && id !== "discs_fe2") return;
  const path = state[id];
  if (!path) {
    setPeek(id, null);
    return;
  }
  try {
    const r = await api("/api/peek", { path });
    setPeek(id, r.peek || r);
  } catch (_) { /* typed path may not exist yet */ }
}

async function refresh() {
  const s = await api("/api/state");
  $("version").textContent = "version " + (s.version || "setup");
  $("os_badge").textContent = s.os.label;
  if (s.in_tree) {
    document.body.classList.add("in-tree");
    if (s.game_folder) setPath("game_folder", s.game_folder);
    const lead = $("lead");
    if (lead && !state.busy) {
      lead.textContent = "This folder is the port. Pick the remaster pack and your disc folders, then Install.";
    }
  }
  if (!state.game_folder && s.game_folder) setPath("game_folder", s.game_folder);
  if (s.phase === "done" && s.ok) {
    setProgress(1);
    setStatus("ok", s.status || "Ready. Start the game from its Desktop shortcut.");
  } else if (s.busy) {
    setProgress(s.progress || 0);
    setStatus("busy", s.status || ("Installing… " + (s.phase || "")));
  } else if (s.error) {
    setProgress(s.progress || 0);
    setStatus("err", s.error);
  }
  if (s.log && s.log.length) {
    $("log").textContent = s.log.join("\n");
    if (state.logOpen) $("log").scrollTop = $("log").scrollHeight;
  }
  document.body.classList.toggle("busy", !!s.busy);
  $("install").disabled = !!s.busy;
  PATH_IDS.forEach((id) => { $(id).disabled = !!s.busy; });
  state.busy = !!s.busy;
}

document.querySelectorAll("[data-browse]").forEach((btn) => {
  btn.addEventListener("click", async () => {
    try {
      const kind = btn.dataset.browse;
      const r = await api("/api/browse", { kind });
      const id = BROWSE_TO[kind] || kind;
      if (r.path) setPath(id, r.path, r.peek);
    } catch (e) { setStatus("err", e.message); }
  });
});

PATH_IDS.forEach((id) => {
  $(id).addEventListener("input", () => {
    state[id] = $(id).value.trim();
    clearTimeout(peekTimer[id]);
    peekTimer[id] = setTimeout(() => peekPath(id), 400);
    if ($("status").classList.contains("err") && state.game_folder && state.assets && (state.discs_fe1 || state.discs_fe2)) {
      setStatus("", "");
    }
  });
});

$("assets_link").addEventListener("click", (ev) => {
  ev.preventDefault();
  api("/api/open", { url: "https://feremaster.us/downloads" }).catch(() => {
    window.open("https://feremaster.us/downloads", "_blank");
  });
});

function closeSetup() {
  try { window.close(); } catch (e) {}
  try { window.open("", "_self"); window.close(); } catch (e) {}
}

$("quit").addEventListener("click", () => {
  fetch("/api/quit", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: "{}",
    keepalive: true,
  }).catch(() => {});
  closeSetup();
});
$("toggle_log").addEventListener("click", () => showLog($("logbox").hidden));

$("install").addEventListener("click", async () => {
  PATH_IDS.forEach((id) => { state[id] = $(id).value.trim(); });
  if (!state.game_folder || !state.assets || (!state.discs_fe1 && !state.discs_fe2)) {
    setStatus("err", "Choose the game folder, the asset pack, and discs for at least one game.");
    return;
  }
  setProgress(0.04);
  setStatus("busy", "Starting install…");
  showLog(true);
  try {
    await api("/api/install", {
      game_folder: state.game_folder,
      assets: state.assets,
      discs_fe1: state.discs_fe1,
      discs_fe2: state.discs_fe2,
    });
  } catch (e) { setStatus("err", e.message); }
});

refresh();
setInterval(refresh, 800);
window.addEventListener("load", () => { try { window.resizeTo(720, 440); } catch (e) {} });
