#!/data/data/com.termux/files/usr/bin/node
// homescreen.mjs — EXPERIMENTAL v3: homescreen CLI ala desktop,
// sekelas interface opencode (OpenTUI+Solid: layout header/list/detail,
// fuzzy filter, theme, footer keybind). Nol dependensi, pakai node host.
//
//   ./experiments/homescreen.mjs            # interaktif
//   ./experiments/homescreen.mjs --list     # polos (CI)
//   ./experiments/homescreen.mjs --preview [home|menu|files|settings]
//   ./experiments/homescreen.mjs --ls /root # isi direktori wadah (uji files)
//
// Kontrol home: ketik = filter (huruf apa pun), panah = pindah,
// Enter = jalan, r = pindai ulang, Esc/q = keluar. Tile files = file manager wadah,
// tile settings = pengaturan homescreen (tersimpan di ~/.config).
// Di files: Enter masuk/lihat, Backspace naik, q/Esc kembali.

import { readdirSync, statSync, lstatSync, readlinkSync, accessSync, constants as FS, readFileSync, writeFileSync, mkdirSync, rmdirSync, rmSync, unlinkSync } from "node:fs";
import { basename, join, normalize, sep } from "node:path";
import { spawnSync, spawn } from "node:child_process";
import { existsSync } from "node:fs";

const THOME = existsSync("/data/data/com.termux/files/home")
  ? "/data/data/com.termux/files/home"
  : process.env.HOME;
// penanda sesi desktop (agar .bashrc auto-desktop tak rekursi)
process.env.HS_DESKTOP = "1";
const BASE = process.env.FAKE_BASE || join(THOME, "alpine-rootfs");
const RHOME = join(BASE, "root");
const ALPINE =
  existsSync("/data/data/com.termux/files/usr/bin/alpine")
    ? "/data/data/com.termux/files/usr/bin/alpine"
    : "alpine";
// FR dipakai untuk MELUNCURKAN (bukan `alpine`): `alpine` memaksa --svsp
// yang mati EBUSY di device ini, sementara fake-run auto jatuh ke ldpreload
// untuk biner dinamis (shell/opencode/hermes terverifikasi jalan).
const FR =
  existsSync("/data/data/com.termux/files/usr/bin/fake-run")
    ? "/data/data/com.termux/files/usr/bin/fake-run"
    : "fake-run";

// --- settings (tersimpan, hanya untuk homescreen) ---
// HS_CONFIG untuk uji (default: ~/.config/brainstorming-homescreen.json)
const CFG = process.env.HS_CONFIG || join(THOME, ".config", "brainstorming-homescreen.json");
let SET = { showSmall: false, confirm: false, showRuntimes: false, launchMode: "auto", showHidden: true, history: [] };
let histIdx = -1, histDraft = ""; // navigasi riwayat di dialog Jalankan
try { SET = { ...SET, ...JSON.parse(readFileSync(CFG, "utf8")) }; } catch {}
function saveSet() {
  try { mkdirSync(join(THOME, ".config"), { recursive: true }); } catch {}
  try { writeFileSync(CFG, JSON.stringify(SET, null, 2)); } catch {}
}

// --- path wadah: "/root/x" -> "$BASE/root/x" (tanpa lolos dari BASE) ---
function hostOf(cpath) {
  const rel = normalize("/" + (cpath || "/")).replace(/^\/+/, "");
  const h = normalize(join(BASE, rel));
  if (h !== BASE && !h.startsWith(BASE + sep)) return BASE;
  return h;
}
function listDir(cpath) {
  let ents = [];
  try { ents = readdirSync(hostOf(cpath), { withFileTypes: true }); } catch { return []; }
  const out = [];
  for (const e of ents) {
    if (e.name === "." || e.name === "..") continue;
    if (!SET.showHidden && e.name.startsWith(".")) continue;
    let sz = 0, isDir = e.isDirectory();
    try { sz = statSync(join(hostOf(cpath), e.name)).size; } catch {}
    out.push({ name: e.name + (isDir ? "/" : ""), isDir, size: sz });
  }
  out.sort((a, b) => (b.isDir - a.isDir) || (a.name < b.name ? -1 : 1));
  return out;
}

// --- theme (opencode-ish: gelap + hijau terang + orange aksen) ---
const C = {
  green: "\x1b[1;38;2;0;255;0m",
  line: "\x1b[38;2;0;170;0m",
  orange: "\x1b[38;2;255;135;0m",
  dim: "\x1b[2;38;2;0;170;0m",
  rev: "\x1b[7m",
  off: "\x1b[0m",
};
const noColor = !process.stdout.isTTY;
for (const k of Object.keys(C)) if (noColor) C[k] = "";

// --- tray: baterai (Termux:API bila ada) + proses wadah (cache per rescan) ---
let TRAY = { batt: "", procs: null };
function readTray() {
  let batt = "";
  try {
    const r = spawnSync("termux-battery-status", [], { encoding: "utf8", timeout: 3000 });
    if (r.status === 0 && r.stdout) {
      const j = JSON.parse(r.stdout);
      if (typeof j.percentage === "number") batt = `${j.percentage}%`;
    }
  } catch {}
  let procs = null;
  // PREFIX wajib ada untuk skrip `alpine` (set -u); di shell aneh ia bisa
  // unset — warisi env tapi jamin default Termux. (Bug hulu di `alpine:22`,
  // tidak diubah karena ini eksperimen.)
  const penv = { ...process.env, PREFIX: process.env.PREFIX || "/data/data/com.termux/files/usr" };
  try {
    const r = spawnSync(ALPINE, ["ps"], { encoding: "utf8", timeout: 20000, env: penv });
    if (r.status === 0 && r.stdout) {
      const m = r.stdout.match(/(\d+) proses wadah/);
      if (m) procs = m[1];
      else if (/tidak ada proses/.test(r.stdout)) procs = "0";
    }
  } catch {}
  TRAY = { batt, procs };
}

// --- scan (aturan sama: user-dir selalu masuk, /usr/local/bin >= 1MB) ---
const RUNTIME = new Set([
  "node", "nodejs", "deno", "bun", "python", "python3", "ruby",
  "go", "java", "perl", "php", "lua", "luajit",
]);
const SKIP = new Set([
  "sh","ash","bash","busybox","apk","cat","ls","cp","mv","rm","ln","chmod",
  "mkdir","grep","sed","awk","sort","head","tail","wc","tr","find","xargs",
  "tar","gzip","ps","kill","sleep","date","env","which","id","whoami","echo",
  "printf","test","clear","reset","stty","uname","alpine","app","ps-wrap",
  "node-wrap",
]);
function scan() {
  const tiles = [
    { name: "shell", cmd: [FR, "/bin/sh", "-l"], desc: "shell interaktif wadah", sys: true, env: { FAKE_START: RHOME } },
    { name: "files", cmd: ["INTERNAL:files"], desc: "file manager wadah (/root…)", sys: true },
    { name: "settings", cmd: ["INTERNAL:settings"], desc: "pengaturan homescreen", sys: true },
    // apps: window internal gaya homescreen (daftar di bawah), bukan
    // perintah `app` eksternal yang gaya teksnya beda + rapuh di device
    // EBUSY (shebang /bin/sh berisi sintaks bash). Data sama persis.
    { name: "apps", cmd: ["INTERNAL:apps"], desc: "daftar aplikasi (gaya homescreen)", sys: true },
    { name: "sistem", cmd: ["INTERNAL:sys"], desc: "proses + port + kill (gaya homescreen)", sys: true },
    { name: "jalankan", cmd: ["INTERNAL:run"], desc: "perintah bebas ala Alt+F2", sys: true },
  ];
  const seen = new Set(tiles.map((t) => t.name));
  const seenIno = new Set();
  const dirs = [
    join(RHOME, ".opencode/bin"), join(RHOME, ".local/bin"),
    join(RHOME, ".claude/local"),
    join(RHOME, ".hermes/hermes-agent/.hermes/bin"),
    join(BASE, "usr/local/bin"),
    join(BASE, "opt"),
  ];
  // $RHOME/.hermes/tools/*/bin (node, python, uv, ripgrep) dan /opt/*/bin —
  // tanpa ini node/python kelewat (beda dari `app`: hanya 2 app, harusnya 4)
  for (const parent of [join(RHOME, ".hermes/tools"), join(BASE, "opt")]) {
    let subs = [];
    try { subs = readdirSync(parent, { withFileTypes: true }); } catch { continue; }
    for (const s of subs) {
      if (!s.isDirectory() && !s.isSymbolicLink()) continue;
      const b = join(parent, s.name, "bin");
      let st = null;
      try { st = statSync(b); } catch { continue; }
      if (st && st.isDirectory()) dirs.push(b);
    }
  }
  for (const d of dirs) {
    let ents = [];
    try { ents = readdirSync(d, { withFileTypes: true }); } catch { continue; }
    for (const e of ents) {
      if (!e.isFile() && !e.isSymbolicLink()) continue;
      const bn = e.name;
      if (seen.has(bn) || SKIP.has(bn)) continue;
      if (/\.(so|a|o|py|pl|sh)(\..*)?$/.test(bn)) continue;
      if (/-acp$|-mcp$|-harness$|-agent$|-gen-src$/.test(bn)) continue;
      if (/[0-9]$/.test(bn) || /^pip/.test(bn) || /^idle/.test(bn)) continue;
      if (/-config$/.test(bn)) continue; // helper kompilasi, bukan app
      // runtime bahasa (node/python/go/…) bukan app interface: tetap bisa
      // dipakai lewat tile shell, tapi jangan penuhi grid + window Apps
      // (kecuali setting showRuntimes)
      if (RUNTIME.has(bn) && !SET.showRuntimes) continue;
      const f = join(d, bn);
      let lst = null, st = null;
      try { lst = lstatSync(f); st = statSync(f); } catch { continue; }
      // symlink (npm/npx/pydoc3 → target kecil): buang bila target < 1MB,
      // KECUALI launcher npm-global (../lib/node_modules/*/cli.js): itu app
      // betulan (mis. 9router) walau targetnya kecil. npm/npx sendiri tetap
      // disembunyikan sebagai tool rantai (setara pip yang sudah dibuang).
      // symlink ke busybox = applet, selalu buang.
      if (lst.isSymbolicLink()) {
        let tgt = "";
        try { tgt = String(readlinkSync(f)); } catch {}
        if (/busybox$/.test(tgt)) continue;
        const launcher = /node_modules/.test(tgt) && !/^(npm|npx)$/.test(bn);
        if (st.size < 1048576 && !launcher && !SET.showSmall) continue;
      } else if (!f.startsWith(RHOME) && st.size < 1048576 && !SET.showSmall) {
        continue; // di luar $RHOME hanya yang >= 1 MB
      }
      // x-bit: harus bisa dieksekusi
      try { accessSync(f, FS.X_OK); } catch { continue; }
      // dedupe inode (python/python3/python3.14 = file yang sama)
      if (seenIno.has(st.ino)) continue;
      seenIno.add(st.ino);
      const sz = st.size;
      seen.add(bn);
      // fake-run hanya resolve nama polos via PATH wadah (/bin, /usr/bin…);
      // opencode/hermes di /root/... tak ketemu → "program tak ada".
      // Selalu pakai path absolut wadah.
      const cprog = f.startsWith(RHOME) ? "/root/" + f.slice(RHOME.length + 1)
        : "/" + f.slice(BASE.length + 1).replace(/^\/+/, "");
      tiles.push({
        name: bn,
        cmd: [FR, cprog],
        desc: f.replace(BASE, "").replace(RHOME, "~") || f,
        sys: false,
        sizeMB: Math.floor(sz / 1048576),
      });
    }
  }
  return tiles;
}

// --- ikon + grid ala GUI ---
const GLYPH = {
  shell: "$_", apps: "::", sistem: "S*", jalankan: ">_",
  files: "[]", settings: "=*",
  opencode: "</>", claude: "CC", hermes: "@@",
  node: "JS", python: "PY", npm: "NP", uv: "UV",
};
const glyphOf = (n) => GLYPH[n] || n.slice(0, 2).toUpperCase();
const CARD_W = 20; // 18 isi + 2 border
function gridCols() {
  const w = process.stdout.columns || 80;
  return Math.max(2, Math.min(5, Math.floor(w / CARD_W)));
}
// template perintah bawaan: habis riwayat, panah lanjut ke template
const TEMPLATES = [
  "npm run dev",
  "python3 -m http.server 8000",
  "htop",
  "ls -la",
  "git status",
];
const runPool = () => [
  ...(SET.history || []),
  ...TEMPLATES.filter((t) => !(SET.history || []).includes(t)),
];

// --- lengkapi otomatis baris perintah (ala shell): dari nama tile,
// riwayat, dan perintah umum; Tab = prefiks bersama terpanjang
function autocomplete() {
  const m = inputBuf.match(/(\S*)$/);
  const frag = (m && m[1]) || "";
  if (!frag) return;
  const cands = new Set();
  for (const t of tiles) cands.add(t.name);
  for (const h of SET.history || []) for (const w of h.split(/\s+/)) cands.add(w);
  for (const w of ["ls", "cat", "htop", "npm", "python3", "node", "git", "cd", "pwd", "echo", "top"]) cands.add(w);
  const hits = [...cands].filter((w) => w.startsWith(frag) && w !== frag);
  if (!hits.length) return;
  let prefix = hits[0];
  for (const h of hits) {
    let i = 0;
    while (i < prefix.length && i < h.length && prefix[i] === h[i]) i++;
    prefix = prefix.slice(0, i);
  }
  if (prefix.length > frag.length) {
    inputBuf = inputBuf.slice(0, inputBuf.length - frag.length) + prefix + (hits.length === 1 ? " " : "");
    histIdx = -1;
  }
}

// bingkai window ala GUI: title bar + border
function window(title, body) {
  const w = process.stdout.columns || 80;
  const inner = w - 2;
  const L = [];
  L.push(`${C.green}┌─ ${title} ${"─".repeat(Math.max(0, inner - title.length - 3))}┐${C.off}`);
  for (const ln of body) {
    const plain = ln.replace(/\x1b\[[0-9;]*m/g, "");
    L.push(`${C.green}│${C.off}${ln}${" ".repeat(Math.max(0, inner - plain.length))}${C.green}│${C.off}`);
  }
  L.push(`${C.green}└${"─".repeat(inner)}┘${C.off}`);
  return L;
}

// --- fuzzy sederhana (subsequence, ala fuzzysort ringan) ---
function match(q, name) {
  q = q.toLowerCase(); name = name.toLowerCase();
  let qi = 0;
  for (const ch of name) if (ch === q[qi]) qi++;
  return qi === q.length ? qi : -1;
}

const args = process.argv.slice(2);
if (args.includes("--list")) {
  for (const t of scan()) console.log(`${t.name}|${t.cmd.join(" ")}|${t.desc}`);
  process.exit(0);
}
if (args.includes("--ls")) {
  const p = args[args.indexOf("--ls") + 1] || "/root";
  for (const e of listDir(p)) {
    const sz = e.isDir ? "<dir>" : `${(e.size / 1024).toFixed(1)}K`;
    console.log(`${e.name}|${sz}`);
  }
  process.exit(0);
}

// --- state tampilan: home | files | preview | settings | menu | apps | ports ---
let view = "home";
let msel = 0;
let asel = 0;
let portRows = [];
let psRows = [];
let ysel = 0; // kursor window Sistem gabungan
const sysItems = () => [
  ...psRows.map((p) => ({ kind: "proc", p })),
  ...portRows.map((r) => ({ kind: "port", r })),
  ...jobs.map((j) => ({ kind: "job", j })),
];
const appRows = () => tiles.filter((t) => !t.sys);
// sama seperti `alpine ports`: baca config + probe koneksi (Android
// sembunyikan /proc/net, jadi ss/netstat tak bisa). Disimpan per buka.
function readPorts() {
  const rows = [];
  try {
    const cfg = JSON.parse(readFileSync(join(RHOME, ".config/opencode/service.json"), "utf8"));
    if (cfg && typeof cfg.port === "number") {
      rows.push({ label: "opencode (wadah)", port: cfg.port, note: "dari ~/.config/opencode/service.json", state: probe(cfg.port) });
    }
  } catch {}
  try {
    const px = process.env.PREFIX || "/data/data/com.termux/files/usr";
    const a = spawnSync("pgrep", ["-f", `${px}/bin/opencode`], { encoding: "utf8", timeout: 5000 });
    const b = spawnSync("pgrep", ["-x", "opencode"], { encoding: "utf8", timeout: 5000 });
    if ((a.status === 0 && a.stdout.trim()) || (b.status === 0 && b.stdout.trim())) {
      rows.push({ label: "opencode (host)", port: 49374, note: "proses host terlihat berjalan", state: probe(49374) });
    }
  } catch {}
  portRows = rows;
}
function probe(port) {
  try {
    const r = spawnSync("curl", ["-s", "-o", "/dev/null", "-m", "2", `http://127.0.0.1:${port}/`], { timeout: 5000 });
    return r.status === 0 ? "aktif" : "idle";
  } catch { return "idle"; }
}
// sama seperti `alpine ps`: saring proses host yang perintahnya menyentuh
// $BASE, tampilkan sebagai path wadah. Disimpan per buka/segarkan.
function readPs() {
  psRows = [];
  const penv = { ...process.env, PREFIX: process.env.PREFIX || "/data/data/com.termux/files/usr" };
  try {
    const r = spawnSync(ALPINE, ["ps"], { encoding: "utf8", timeout: 20000, env: penv });
    if (r.status !== 0 || !r.stdout) return;
    for (const ln of r.stdout.split("\n")) {
      const m = ln.match(/^\s*(\d+)\s+(\S+)\s+(.+)$/);
      if (!m || m[1] === "PID") continue;
      // `alpine ps` sudah menyingkat $BASE jadi "(wadah)" — terima keduanya
      if (!m[3].includes(BASE) && !m[3].includes("(wadah)")) continue;
      let cmd = m[3].startsWith(BASE) ? "/" + m[3].slice(BASE.length + 1) : m[3].split(BASE).join("(wadah)");
      cmd = cmd.replace("/lib/ld-musl-patched.so.1 ", "");
      psRows.push({ pid: m[1], stat: m[2], cmd });
    }
  } catch {}
}
let fstate = { cwd: "/root", entries: [], fsel: 0 };
let pstate = { file: "", lines: [] };
let ssel = 0;
function openFiles(cwd) {
  picking = false; // browse biasa; mode petik diset pemanggil sesudah ini
  fstate = { cwd: cwd || "/root", entries: listDir(cwd || "/root"), fsel: 0 };
  view = "files";
}
function openPreview(cpath) {
  let txt = "";
  try { txt = readFileSync(hostOf(cpath), "utf8"); } catch { txt = ""; }
  if (txt === "") {
    pstate = { file: cpath, lines: ["(tak bisa dibaca: biner / tak ada izin / kosong)"] };
  } else if (txt.includes("\0")) {
    let sz = 0;
    try { sz = statSync(hostOf(cpath)).size; } catch {}
    pstate = { file: cpath, lines: [`(file biner, ${(sz / 1024).toFixed(1)}K — tidak ditampilkan)`] };
  } else {
    pstate = { file: cpath, lines: txt.split("\n").slice(0, 40) };
  }
  view = "preview";
}
const SETTINGS_DEF = [
  { key: "showSmall", type: "bool", label: "Tampilkan tool kecil (<1MB)", hint: "ikutkan util kecil /usr/local/bin di pindai" },
  { key: "confirm", type: "bool", label: "Konfirmasi sebelum jalan", hint: "ketik-nama tiap Enter di tile app" },
  { key: "showRuntimes", type: "bool", label: "Tampilkan runtime di Apps", hint: "node/python ikut grid + window Apps (mati = via shell)" },
  { key: "launchMode", type: "choice", choices: ["auto", "svsp", "ldpreload"], label: "Jalur luncur", hint: "auto = fallback cerdas (EBUSY); svsp/ldpreload = paksa" },
  { key: "showHidden", type: "bool", label: "File tersembunyi di Files", hint: "tampilkan dotfile (.bashrc, .git…)" },
];

let tiles = scan();
let jobs = [], jobSeq = 0, curJob = null; // job latar dialog Jalankan
if (!args.includes("--preview")) readTray();
else if (process.stdout.isTTY) readTray();
// --preview non-TTY: tanpa tray (tetap cepat, tanpa boot wadah)
let query = "", sel = 0;
const filtered = () =>
  query ? tiles.filter((t) => match(query, t.name) >= 0) : tiles;

function frame() {
  if (view === "files") return frameFiles();
  if (view === "preview") return framePreview();
  if (view === "settings") return frameSettings();
  if (view === "menu") return frameMenu();
  if (view === "apps") return frameApps();
  if (view === "sys") return frameSys();
  if (view === "job") return frameJob();
  if (view === "run") return frameRun();
  if (view === "confirm") return frameConfirm();
  if (view === "notice") return frameNotice();
  return frameHome();
}

function topbar(title) {
  const w = process.stdout.columns || 80;
  const now = new Date().toTimeString().slice(0, 5);
  let alpineV = "?";
  try { alpineV = readFileSync(join(BASE, "etc/alpine-release"), "utf8").trim(); } catch {}
  const trayBits = [`${tiles.length} app`];
  if (TRAY.procs !== null) trayBits.push(`${TRAY.procs} tugas`);
  trayBits.push(now);
  if (TRAY.batt) trayBits.push(TRAY.batt);
  const status = `Alpine ${alpineV} · ${trayBits.join(" · ")} `;
  return [
    `${C.rev} ${title} ${C.off}${" ".repeat(Math.max(1, w - title.length - 4 - status.length))}${C.dim}${status}${C.off}`,
    `${C.line}${"─".repeat(w)}${C.off}`,
  ];
}

function frameFiles() {
  const body = [];
  const rows = Math.max(1, (process.stdout.rows || 24) - 8);
  const shown = fstate.entries.slice(0, rows);
  if (!shown.length) body.push(`${C.dim}(kosong / tak bisa dibaca)${C.off}`);
  for (let i = 0; i < shown.length; i++) {
    const e = shown[i];
    const sz = e.isDir ? "<dir>" : `${(e.size / 1024).toFixed(1)}K`;
    const nm = i === fstate.fsel ? `${C.green}${e.name}${C.off}` : e.name;
    body.push(`${i === fstate.fsel ? `${C.rev}▸` : " "} ${nm}${i === fstate.fsel ? C.off : ""}${" ".repeat(Math.max(1, 30 - e.name.length))}${C.dim}${sz}${C.off}`);
  }
  body.push(picking
    ? `${C.dim}Enter petik folder · →/l masuk · Spasi folder ini · Backspace naik · Esc/q batal${C.off}`
    : `${C.dim}Enter masuk/lihat · Esc/Backspace naik · d hapus · ! perintah di sini · q home${C.off}`);
  const winTitle = picking ? `Petik folder → ${fstate.cwd}` : `Files ${fstate.cwd}`;
  return [...topbar("Brainstorming Desktop"), ...window(winTitle, body)].join("\n");
}

function framePreview() {
  const w = process.stdout.columns || 80;
  const body = pstate.lines
    .slice(0, Math.max(1, (process.stdout.rows || 24) - 8))
    .map((ln) => ln.slice(0, w - 4));
  body.push(`${C.dim}q/Esc kembali ke files${C.off}`);
  return [...topbar("Brainstorming Desktop"), ...window("Lihat " + pstate.file, body)].join("\n");
}

function frameMenu() {
  const items = [
    ...filtered().map((t) => ({ label: t.name, hint: t.desc, tile: t })),
    { label: "—", hint: "", tile: null },
    { label: "Pindai ulang", hint: "scan app + tray", tile: "rescan" },
    { label: "Keluar", hint: "tutup homescreen", tile: "quit" },
  ];
  if (msel >= items.length) msel = items.length - 1;
  const body = items.map((it, i) => {
    if (!it.tile) return `${C.dim}──────────────────${C.off}`;
    const nm = i === msel ? `${C.green}${it.label}${C.off}` : it.label;
    return `${i === msel ? `${C.rev}▸` : " "} ${nm}${i === msel ? C.off : ""}  ${C.dim}${it.hint}${C.off}`;
  });
  body.push(`${C.dim}↑↓ pilih · Enter jalankan · Esc/Tab tutup${C.off}`);
  return [...topbar("Brainstorming Desktop"), ...window("Start", body)].join("\n");
}

function frameApps() {
  const rows = appRows();
  if (asel >= rows.length) asel = Math.max(0, rows.length - 1);
  const body = [];
  body.push(`${C.dim}  NAMA           UKURAN   LOKASI${C.off}`);
  rows.forEach((t, i) => {
    const nm = i === asel ? `${C.green}${t.name}${C.off}` : t.name;
    const size = `${t.sizeMB ?? 0} MB`.padStart(7);
    body.push(`${i === asel ? `${C.rev}▸` : " "} ${nm}${i === asel ? C.off : ""}${" ".repeat(Math.max(1, 15 - t.name.length))}${size}  ${C.dim}${t.desc}${C.off}`);
  });
  if (!rows.length) body.push(`${C.dim}(belum ada aplikasi — taruh biner di /usr/local/bin wadah)${C.off}`);
  body.push(`${C.dim}${rows.length} aplikasi · Enter jalankan · r pindai ulang · q/Esc kembali${C.off}`);
  return [...topbar("Brainstorming Desktop"), ...window("Apps", body)].join("\n");
}

// --- dialog seirama GUI: konfirmasi ketik-nama + notice hasil ---
// Tak ada lagi prompt teks polos di luar window.
let confirmState = null; // {title, lines[], expect, onYes}
let noticeState = null;  // {title, lines[], back}
let inputBuf = "";
let inputBuf2 = "/root"; // folder dialog Jalankan (path wadah)
let rfocus = 0;          // 0 = perintah, 1 = folder
let picking = false;     // Files mode petik-folder untuk dialog Jalankan
function askConfirm(st) { confirmState = st; inputBuf = ""; view = "confirm"; }
function showNotice(st) { noticeState = st; view = "notice"; }
function frameConfirm() {
  const body = [
    ...confirmState.lines,
    "",
    `> ${inputBuf}▌`,
    `${C.dim}ketik "${confirmState.expect}" + Enter = ya · Esc = batal${C.off}`,
  ];
  return [...topbar("Brainstorming Desktop"), ...window(confirmState.title, body)].join("\n");
}
function frameNotice() {
  const body = [...noticeState.lines, "", `${C.dim}Enter = kembali${C.off}`];
  return [...topbar("Brainstorming Desktop"), ...window(noticeState.title, body)].join("\n");
}

// --- dialog Jalankan (Alt+F2): perintah bebas di wadah ---
// tokenizer mini: spasi pemisah, "..."/'...' literal
function shSplit(s) {
  const out = [];
  let cur = "", q = null;
  for (const ch of s) {
    if (q) { if (ch === q) q = null; else cur += ch; }
    else if (ch === '"' || ch === "'") q = ch;
    else if (/\s/.test(ch)) { if (cur) { out.push(cur); cur = ""; } }
    else cur += ch;
  }
  if (cur) out.push(cur);
  return out;
}
function frameRun() {
  const body = [
    `${C.dim}contoh: npm run dev · python3 -m http.server 8000 (dari Files: ! = folder ini)${C.off}`,
    "",
    `> ${rfocus === 0 ? inputBuf + "▌" : inputBuf}`,
    `@ ${rfocus === 1 ? inputBuf2 + "▌" : inputBuf2}`,
    `${C.dim}Tab lengkap · → folder · ← perintah · ↑↓ template · Enter @ petik · Enter > jalan · Esc batal${C.off}`,
  ];
  return [...topbar("Brainstorming Desktop"), ...window("Jalankan", body)].join("\n");
}

function frameSys() {
  const items = sysItems();
  if (ysel >= items.length) ysel = Math.max(0, items.length - 1);
  const body = [];
  body.push(`${C.dim}─ PROSES ─${C.off}`);
  if (!psRows.length) body.push(`${C.dim}(tidak ada proses wadah yang jalan)${C.off}`);
  let idx = 0;
  for (const p of psRows) {
    const row = `${p.pid.padEnd(8)} ${p.stat.padEnd(5)} ${p.cmd}`;
    body.push(`${idx === ysel ? `${C.rev}▸` : " "} ${idx === ysel ? `${C.green}${row}${C.off}` : row}${idx === ysel ? C.off : ""}`);
    idx++;
  }
  body.push(`${C.dim}─ PORT ─${C.off}`);
  if (!portRows.length) body.push(`${C.dim}(tidak ada port app terdaftar)${C.off}`);
  const seen = new Map();
  for (const r of portRows) seen.set(r.port, [...(seen.get(r.port) || []), r.label]);
  for (const r of portRows) {
    const dot = r.state === "aktif" ? `${C.green}● aktif${C.off}` : `${C.dim}○ idle${C.off}`;
    body.push(`${idx === ysel ? `${C.rev}▸` : " "} ${C.orange}${r.label}${C.off}  ${r.port}  ${dot}${idx === ysel ? C.off : ""}`);
    idx++;
  }
  for (const [p, ls] of seen) {
    if (ls.length > 1) body.push(`${C.orange}⚠ bentrok: ${ls.join(" dan ")} sama-sama memakai port ${p}${C.off}`);
  }
  if (jobs.length) {
    body.push(`${C.dim}─ JOB LATAR ─${C.off}`);
    for (const j of jobs) {
      const st = j.done ? `${C.dim}selesai ${j.done}${C.off}` : `${C.green}● jalan${C.off}`;
      body.push(`${idx === ysel ? `${C.rev}▸` : " "} ${idx === ysel ? `${C.green}#${j.id} ${j.label}${C.off}` : `#${j.id} ${j.label}`}${idx === ysel ? C.off : ""}  ${st}`);
      idx++;
    }
  }
  const cur = items[ysel];
  const hint = cur && cur.kind === "proc" ? "Enter kill proses" : cur && cur.kind === "job" ? "Enter lihat output" : "Enter segarkan port";
  body.push(`${C.dim}${psRows.length} proses · ${portRows.length} port · ${hint} · r segarkan semua · q/Esc kembali${C.off}`);
  return [...topbar("Brainstorming Desktop"), ...window("Sistem", body)].join("\n");
}

function frameSettings() {  const body = [];
  SETTINGS_DEF.forEach((s, i) => {
    const val = s.type === "choice" ? `<${SET[s.key] || s.choices[0]}>` : (SET[s.key] ? "[x]" : "[ ]");
    const nm = i === ssel ? `${C.green}${s.label}${C.off}` : s.label;
    body.push(`${i === ssel ? `${C.rev}▸` : " "} ${val} ${nm}${i === ssel ? C.off : ""}  ${C.dim}${s.hint}${C.off}`);
  });
  body.push(`${C.dim}Enter/Spasi ubah · q/Esc kembali (tersimpan di ~/.config/brainstorming-homescreen.json)${C.off}`);
  return [...topbar("Brainstorming Desktop"), ...window("Settings", body)].join("\n");
}

function frameHome() {
  const cols = gridCols();
  const list = filtered();
  if (sel >= list.length) sel = Math.max(0, list.length - 1);
  const cur = list[sel];
  const L = topbar("Brainstorming Desktop");
  // --- area desktop: grid ikon kotak ala GUI ---
  for (let i = 0; i < list.length; i += cols) {
    const row = list.slice(i, i + cols);
    // garis atas kartu
    L.push(row.map((_, k) =>
      (i + k === sel ? C.green : C.line) +
      "┌──────────────────┐" + C.off).join(" "));
    // glyph besar
    L.push(row.map((t, k) => {
      const g = `[ ${glyphOf(t.name)} ]`;
      const pad = 18 - g.length, pl = Math.floor(pad / 2), pr = pad - pl;
      const b = i + k === sel ? C.green : C.line;
      const tx = i + k === sel ? C.rev + g + C.off + b : g;
      return `${b}│${C.off}${" ".repeat(pl)}${tx}${" ".repeat(pr)}${b}│${C.off}`;
    }).join(" "));
    // nama rata tengah
    L.push(row.map((t, k) => {
      const nm = t.name.slice(0, 16);
      const pad = 18 - nm.length, pl = Math.floor(pad / 2), pr = pad - pl;
      const b = i + k === sel ? C.green : C.line;
      const tx = i + k === sel ? C.green + nm + C.off + b : nm;
      return `${b}│${C.off}${" ".repeat(pl)}${tx}${" ".repeat(pr)}${b}│${C.off}`;
    }).join(" "));
    // garis bawah kartu
    L.push(row.map((_, k) =>
      (i + k === sel ? C.green : C.line) +
      "└──────────────────┘" + C.off).join(" "));
  }
  if (!list.length) L.push(`${C.dim}(tidak ada yang cocok — hapus filter dengan backspace)${C.off}`);
  // --- taskbar bawah ala GUI ---
  const w = process.stdout.columns || 80;
  L.push(`${C.line}${"─".repeat(w)}${C.off}`);
  const q = query ? ` cari:${query}▌` : "";
  L.push(`${C.orange} Start${C.off}${C.dim} │${q} ${cur ? `${C.green}${cur.name}${C.off} ${cur.desc}` : "(kosong)"} │ Tab menu · ↑↓←→ · Enter buka · q keluar${C.off}`);
  return L.join("\n");
}
if (args.includes("--preview")) {
  const which = args[args.indexOf("--preview") + 1] || "home";
  if (which === "menu") view = "menu";
  else if (which === "settings") view = "settings";
  else if (which === "apps") view = "apps";
  else if (which === "sys") { readPs(); readPorts(); view = "sys"; }
  else if (which === "run") { inputBuf = "python3 -m http.server 8000"; inputBuf2 = "/root"; view = "run"; }
  else if (which === "job") {
    // demo tanpa proses beneran (CI): satu baris contoh
    const demo = { id: 1, label: "demo", pid: 1234, proc: { pid: 1234 }, buf: ["$ contoh output server", "Serving on 8000…"], done: null };
    jobs = [demo]; curJob = demo; view = "job";
  }
  else if (which === "confirm") {
    askConfirm({ title: "Hapus", lines: ["/root/contoh (+3 isi, rekursif)"], expect: "contoh", back: "files", onYes: () => {} });
    inputBuf = "con";
  } else if (which === "notice") {
    showNotice({ title: "Hapus", lines: ["/root/contoh terhapus."], back: "files" });
  }
  else if (which === "files") openFiles("/root");
  console.log(frame());
  process.exit(0);
}

// --- interaktif (alternate screen + raw input) ---
if (!process.stdin.isTTY || !process.stdout.isTTY) {
  for (const t of tiles) console.log(`${t.name}|${t.cmd.join(" ")}|${t.desc}`);
  process.exit(0);
}
process.stdout.write("\x1b[?1049h\x1b[?25l");
const restore = () => process.stdout.write("\x1b[?25h\x1b[?1049l\x1b[0m\n");
process.on("exit", restore);
process.stdin.setRawMode(true);
process.stdin.resume();
process.stdin.setEncoding("utf8");
const redraw = () => process.stdout.write("\x1b[H\x1b[2J" + frame());

redraw();
// detak desktop (untuk auto-desktop tab baru): disentuh tiap 5 dtk;
// tab baru anggap "jalan" bila umurnya < 15 dtk. Mati total (nge-hang,
// di-kill, terminal lenyap) = detak basi = diabaikan. Tak ada lockfile
// yang yatim, tak ada false-positive proses hantu.
const BEAT = join(THOME, ".cache", "brainstorming-homescreen.beat");
try { mkdirSync(join(THOME, ".cache"), { recursive: true }); } catch {}
const beat = () => { try { writeFileSync(BEAT, String(Date.now())); } catch {} };
beat();
setInterval(() => { beat(); if (view === "job") redraw(); }, 5000).unref();
let escBuf = "";
function launch(tile) {
  let cmd = buildCmd(tile.cmd);
  const label = tile.name;
  if (SET.confirm) {
    askConfirm({
      title: "Jalankan",
      lines: [`${label}`, `${C.dim}${cmd.join(" ")}${C.off}`],
      expect: label,
      back: "home",
      onYes: () => doLaunch(cmd, tile.env),
    });
    redraw();
    return;
  }
  doLaunch(cmd, tile.env);
}
function doLaunch(cmd, extraEnv) {
  // keluar alt-screen + sane terminal agar program interaktif (shell/TUI)
  // dapat TTY normal; begitu keluar, LANGSUNG kembali (tanpa jeda Enter)
  process.stdout.write("\x1b[?25h\x1b[?1049l");
  process.stdin.setRawMode(false);
  const penv = { ...process.env, PREFIX: process.env.PREFIX || "/data/data/com.termux/files/usr", ...(extraEnv || {}) };
  spawnSync(cmd[0], cmd.slice(1), { stdio: "inherit", env: penv });
  process.stdin.setRawMode(true);
  tiles = scan(); readTray();
  process.stdout.write("\x1b[?1049h\x1b[?25l");
  redraw();
}
// setting jalur luncur: selipkan --svsp/--ldpreload (auto = tanpa flag)
function buildCmd(cmd) {
  if (cmd[0] === FR && SET.launchMode !== "auto") return [FR, `--${SET.launchMode}`, ...cmd.slice(1)];
  return cmd;
}
// job latar: perintah jalan background, output mengalir ke window GUI
// (tetap di desktop, tak dilempar ke Termux polos). Kap 10 job, 300 baris.
function penv(extra) {
  return { ...process.env, PREFIX: process.env.PREFIX || "/data/data/com.termux/files/usr", ...(extra || {}) };
}
function startJob(label, cmdArr, cwdHost, extraEnv) {
  const cmd = buildCmd(cmdArr);
  let proc;
  try {
    proc = spawn(cmd[0], cmd.slice(1), { cwd: cwdHost, env: penv(extraEnv) });
  } catch (e) {
    showNotice({ title: "Jalankan", lines: [`Gagal start: ${e.message}`], back: "home" });
    redraw(); return;
  }
  const job = { id: ++jobSeq, label, proc, pid: proc.pid, buf: [], done: null };
  const push = (d) => {
    for (const ln of String(d).split("\n")) {
      const clean = ln.replace(/\r$/, "").slice(0, 500);
      job.buf.push(clean);
      if (job.buf.length > 300) job.buf.shift();
    }
    if (view === "job" && curJob === job) redraw();
  };
  if (proc.stdout) proc.stdout.on("data", push);
  if (proc.stderr) proc.stderr.on("data", push);
  proc.on("exit", (code, sig) => {
    job.done = code === null ? `signal ${sig}` : `rc=${code}`;
    if (view === "job" && curJob === job) redraw();
  });
  proc.on("error", (e) => {
    job.buf.push(`(gagal start: ${e.message})`);
    job.done = "gagal";
    if (view === "job" && curJob === job) redraw();
  });
  jobs.unshift(job); jobs = jobs.slice(0, 10);
  curJob = job; view = "job"; redraw();
}
function frameJob() {
  if (!curJob) return frameHome();
  const w = process.stdout.columns || 80;
  const rows = Math.max(1, (process.stdout.rows || 24) - 9);
  const tail = curJob.buf.slice(-rows);
  const body = tail.map((ln) => ln.slice(0, w - 4));
  if (!body.length) body.push(`${C.dim}(belum ada output…)${C.off}`);
  const st = curJob.done ? `${C.dim}selesai ${curJob.done}${C.off}` : `${C.green}● jalan${C.off} (PID ${curJob.pid})`;
  body.push(`${st}  ${C.dim}q desktop (tetap jalan) · k matikan · r segarkan${C.off}`);
  return [...topbar("Brainstorming Desktop"), ...window(`Output: ${curJob.label}`, body)].join("\n");
}

// kill: dialog ketik-PID; TERM dulu, -9 bila bandel; hasil di window
function killFlow(p) {
  askConfirm({
    title: "Kill proses",
    lines: [`PID: ${p.pid}`, `CMD: ${p.cmd.slice(0, 60)}`],
    expect: p.pid,
    back: "sys",
    onYes: () => {
      let r = spawnSync("kill", [p.pid]);
      if (r.status !== 0) r = spawnSync("kill", ["-9", p.pid]);
      readPs(); readTray();
      showNotice({
        title: "Kill proses",
        lines: [r.status === 0 ? `PID ${p.pid} dimatikan.` : `Gagal mematikan ${p.pid} (rc=${r.status}).`],
        back: "sys",
      });
    },
  });
  redraw();
}
// hapus hanya di area user (/root, /tmp, /home); folder tak kosong ditolak;
// selalu konfirmasi. Kembali dengan jeda baca hasil (destruktif, langka).
function deleteFlow(cwd, entry) {
  const cpath = (cwd === "/" ? `/${entry.name}` : `${cwd}/${entry.name}`).replace(/\/$/, "");
  const deny = (msg) => { showNotice({ title: "Hapus", lines: [msg], back: "files" }); redraw(); };
  const okArea = ["/root", "/tmp", "/home"].some((a) => cpath === a || cpath.startsWith(a + "/"));
  if (!okArea) return deny(`Tak hapus ${cpath}: di luar area user (/root, /tmp, /home).`);
  if (["/root", "/tmp", "/home"].includes(cpath)) return deny(`Tak hapus ${cpath}: akar area user tak boleh dihapus.`);
  const isDir = entry.isDir;
  const kids = isDir ? listDir(cpath).length : 0;
  const plain = entry.name.replace(/\/$/, "");
  askConfirm({
    title: "Hapus",
    lines: [`${cpath}${kids ? ` (+${kids} isi, rekursif)` : ""}`],
    expect: plain,
    back: "files",
    onYes: () => {
      try {
        const h = hostOf(cpath);
        if (isDir) rmSync(h, { recursive: true, force: true }); else unlinkSync(h);
        fstate.entries = listDir(fstate.cwd);
        showNotice({ title: "Hapus", lines: [`${cpath} terhapus.`], back: "files" });
      } catch (e) {
        showNotice({ title: "Hapus", lines: [`Gagal: ${e.message || e}.`], back: "files" });
      }
    },
  });
  redraw();
}
function upOne(cwd) {
  if (cwd === "/") return "/";
  const parts = cwd.split("/").filter(Boolean);
  parts.pop();
  return "/" + parts.join("/");
}
// input bisa datang segabungan (paste/tekan cepat): pecah jadi per tombol.
// Tanpa ini, "jalankan" sekaligus atau 5×panah cepat = 1 chunk tak dikenal.
function* keys(chunk) {
  let i = 0;
  while (i < chunk.length) {
    if (chunk[i] === "\x1b" && chunk[i + 1] === "[") {
      let j = i + 2;
      while (j < chunk.length && !/[A-Za-z~]/.test(chunk[j])) j++;
      if (j < chunk.length) { yield chunk.slice(i, j + 1); i = j + 1; }
      else { yield "\x1b"; i++; }
    } else { yield chunk[i]; i++; }
  }
}
const handleKey = (k) => {
  if (k === "\x03") { restore(); process.exit(0); }
  // --- dialog Jalankan ---
  if (view === "run") {
    if (k === "\x1b") { inputBuf = ""; inputBuf2 = ""; view = "home"; redraw(); return; }
    if (k === "\t") {
      // Tab = lengkapi otomatis (tak pernah pindah fokus diam-diam)
      if (rfocus === 0) autocomplete();
      else rfocus = 0;
      redraw(); return;
    }
    const cur = () => (rfocus === 0 ? "inputBuf" : "inputBuf2");
    if (k === "\x7f" || k === "\b") {
      if (rfocus === 0) { inputBuf = inputBuf.slice(0, -1); histIdx = -1; }
      else inputBuf2 = inputBuf2.slice(0, -1);
      redraw(); return;
    }
    if (k === "\r" || k === "\n") {
      if (rfocus === 1) {
        // Enter di baris folder = petik via Files (bukan loncat fokus)
        openFiles(inputBuf2 && inputBuf2.startsWith("/") ? inputBuf2 : "/root");
        picking = true;
        redraw(); return;
      }
      const toks = shSplit(inputBuf);
      // simpan riwayat (maks 20, unik, terbaru dulu)
      const raw = inputBuf.trim();
      if (raw) {
        SET.history = [raw, ...(SET.history || []).filter((h) => h !== raw)].slice(0, 20);
        saveSet();
      }
      histIdx = -1; histDraft = "";
      // folder: validasi tetap di wadah, fallback /root
      let dir = (inputBuf2 || "/root").trim() || "/root";
      if (!dir.startsWith("/")) dir = "/root/" + dir;
      const host = hostOf(dir);
      dir = host === BASE ? "/" : "/" + host.slice(BASE.length + 1);
      inputBuf = ""; inputBuf2 = ""; rfocus = 0;
      if (!toks.length) { view = "home"; redraw(); return; }
      // latar + output di window (tak dilempar ke Termux); butuh TTY
      // beneran (shell/TUI)? pakai tile-nya, bukan dialog ini.
      startJob(toks.join(" "), [FR, ...toks], host, { FAKE_START: host });
      return;
    }
    if (/^[\x20-\x7e]$/.test(k)) {
      if (rfocus === 0 && inputBuf.length < 200) { inputBuf += k; histIdx = -1; }
      if (rfocus === 1 && inputBuf2.length < 120) inputBuf2 += k;
      redraw(); return;
    }
    // ↑↓ di baris perintah = riwayat lalu template (ala shell)
    if (k === "\x1b[A" || k === "\x1b[B") {
      const pool = runPool();
      if (rfocus !== 0 || !pool.length) return;
      if (histIdx === -1 && k === "\x1b[A") histDraft = inputBuf;
      histIdx += (k === "\x1b[A" ? 1 : -1);
      histIdx = Math.max(-1, Math.min(pool.length - 1, histIdx));
      inputBuf = histIdx === -1 ? histDraft : pool[histIdx];
      redraw(); return;
    }
    // → = ke baris folder (dari perintah) / petik via Files (dari folder)
    if (k === "\x1b[C") {
      if (rfocus === 0) { rfocus = 1; redraw(); return; }
      openFiles(inputBuf2 && inputBuf2.startsWith("/") ? inputBuf2 : "/root");
      picking = true;
      redraw(); return;
    }
    // ← di baris folder = kembali ke perintah
    if (k === "\x1b[D" && rfocus === 1) { rfocus = 0; redraw(); return; }
    return;
  }
  // --- dialog konfirmasi ketik-nama ---
  if (view === "confirm") {
    if (k === "\x1b") { view = confirmState.back || "home"; confirmState = null; redraw(); return; }
    if (k === "\x7f" || k === "\b") { inputBuf = inputBuf.slice(0, -1); redraw(); return; }
    if (k === "\r" || k === "\n") {
      const st = confirmState; confirmState = null;
      if (inputBuf.trim() === st.expect) { st.onYes(); }
      else { showNotice({ title: st.title, lines: ["Batal."], back: st.back || "home" }); }
      inputBuf = "";
      redraw(); return;
    }
    if (/^[\x20-\x7e]$/.test(k) && inputBuf.length < 64) { inputBuf += k; redraw(); return; }
    return;
  }
  // --- notice hasil ---
  if (view === "notice") {
    if (k === "\r" || k === "\n" || k === "q" || k === "\x1b" || k === " ") {
      const back = (noticeState && noticeState.back) || "home";
      if (back === "sys") { readPs(); readPorts(); readTray(); }
      if (back === "files") { fstate.entries = listDir(fstate.cwd); }
      noticeState = null; view = back; redraw(); return;
    }
    return;
  }
  // --- files (browse biasa, atau mode petik-folder untuk Jalankan) ---
  if (view === "files") {
    const n = fstate.entries.length;
    const cancelPick = () => { picking = false; view = "run"; rfocus = 0; redraw(); };
    if (k === "q") { if (picking) return cancelPick(); view = "home"; redraw(); return; }
    if (k === "\x1b[A" || k === "k") { fstate.fsel = Math.max(0, fstate.fsel - 1); redraw(); return; }
    if (k === "\x1b[B" || k === "j") { fstate.fsel = Math.min(n - 1, fstate.fsel + 1); redraw(); return; }
    if (picking) {
      // Enter = petik (dir disorot → dir itu; file → foldernya; Spasi = folder ini)
      if (k === "\r" || k === "\n") {
        const e = fstate.entries[fstate.fsel];
        if (!e) { inputBuf2 = fstate.cwd; }
        else if (e.isDir) {
          const nm = e.name.replace(/\/$/, "");
          inputBuf2 = fstate.cwd === "/" ? `/${nm}` : `${fstate.cwd}/${nm}`;
        } else inputBuf2 = fstate.cwd; // file → petik foldernya
        picking = false; view = "run"; rfocus = 0; redraw(); return;
      }
      if (k === " ") { inputBuf2 = fstate.cwd; picking = false; view = "run"; rfocus = 0; redraw(); return; }
      if (k === "\x1b" || k === "\x7f" || k === "\b") {
        if (k === "\x1b") return cancelPick();
        openFiles(upOne(fstate.cwd)); redraw(); return;
      }
    }
    // Esc/Backspace/h = naik (di root "/" tetap di "/"); q = kembali home
    if (k === "\x7f" || k === "\b" || k === "\x1b" || k === "\x1b[D" || k === "h") {
      openFiles(upOne(fstate.cwd)); redraw(); return;
    }
    if (k === "\r" || k === "\n" || k === "\x1b[C" || k === "l") {
      const e = fstate.entries[fstate.fsel];
      if (!e) return;
      const next = fstate.cwd === "/" ? `/${e.name}` : `${fstate.cwd}/${e.name}`;
      if (e.isDir) { openFiles(next); redraw(); }
      else openPreview(next), redraw();
      return;
    }
    if (k === "d" || k === "\x1b[3~") {
      const e = fstate.entries[fstate.fsel];
      if (e) deleteFlow(fstate.cwd, e);
      return;
    }
    // ! = jalankan perintah di folder ini (template folder terisi)
    if (k === "!") { inputBuf = ""; inputBuf2 = fstate.cwd; rfocus = 0; view = "run"; redraw(); return; }
    if (k === "r") { fstate.entries = listDir(fstate.cwd); redraw(); return; }
    return;
  }
  // --- preview ---
  if (view === "preview") {
    if (k === "q" || k === "\x1b" || k === "\r" || k === "\n" || k === "\x7f" || k === "\b") {
      view = "files"; redraw(); return;
    }
    return;
  }
  // --- settings ---
  if (view === "settings") {
    if (k === "q" || k === "\x1b") { view = "home"; redraw(); return; }
    if (k === "\x1b[A" || k === "k") { ssel = Math.max(0, ssel - 1); redraw(); return; }
    if (k === "\x1b[B" || k === "j") { ssel = Math.min(SETTINGS_DEF.length - 1, ssel + 1); redraw(); return; }
    if (k === "\r" || k === "\n" || k === " ") {
      const s = SETTINGS_DEF[ssel];
      if (s.type === "choice") {
        const i = s.choices.indexOf(SET[s.key]);
        SET[s.key] = s.choices[(i + 1) % s.choices.length];
      } else {
        SET[s.key] = !SET[s.key];
      }
      saveSet();
      if (s.key === "showSmall" || s.key === "showRuntimes") tiles = scan();
      redraw(); return;
    }
    return;
  }
  // --- apps (daftar internal seirama GUI) ---
  if (view === "apps") {
    const rows = appRows();
    if (k === "q" || k === "\x1b") { view = "home"; redraw(); return; }
    if (k === "\x1b[A" || k === "k") { asel = Math.max(0, asel - 1); redraw(); return; }
    if (k === "\x1b[B" || k === "j") { asel = Math.min(rows.length - 1, asel + 1); redraw(); return; }
    if (k === "r") { tiles = scan(); readTray(); redraw(); return; }
    if (k === "\r" || k === "\n") {
      const t = rows[asel];
      if (!t) return;
      launch(t);
      return;
    }
    return;
  }
  // --- sistem gabungan (proses + port) ---
  if (view === "sys") {
    const items = sysItems();
    if (k === "q" || k === "\x1b") { view = "home"; redraw(); return; }
    if (k === "\x1b[A" || k === "k") { ysel = Math.max(0, ysel - 1); redraw(); return; }
    if (k === "\x1b[B" || k === "j") { ysel = Math.min(items.length - 1, ysel + 1); redraw(); return; }
    if (k === "r") { readPs(); readPorts(); readTray(); redraw(); return; }
    if (k === "\r" || k === "\n") {
      const cur = items[ysel];
      if (!cur) return;
      if (cur.kind === "proc") { killFlow(cur.p); return; }
      if (cur.kind === "job") { curJob = cur.j; view = "job"; redraw(); return; }
      readPorts(); redraw(); return;
    }
    return;
  }
  // --- output job latar ---
  if (view === "job") {
    if (k === "q" || k === "\x1b") { view = "home"; redraw(); return; }
    if (k === "r") { redraw(); return; }
    if (k === "k" || k === "\r" || k === "\n") {
      if (curJob && !curJob.done) killFlow({ pid: String(curJob.proc.pid), cmd: curJob.label });
      return;
    }
    return;
  }
  // --- menu Start ---
  if (view === "menu") {
    const items = [
      ...filtered(),
      "—",
      { name: "Pindai ulang", cmd: ["INTERNAL:rescan"] },
      { name: "Keluar", cmd: ["INTERNAL:quit"] },
    ];
    if (k === "q" || k === "\x1b" || k === "\t") { view = "home"; redraw(); return; }
    if (k === "\x1b[A" || k === "k") { do { msel = (msel - 1 + items.length) % items.length; } while (items[msel] === "—"); redraw(); return; }
    if (k === "\x1b[B" || k === "j") { do { msel = (msel + 1) % items.length; } while (items[msel] === "—"); redraw(); return; }
    if (k === "\r" || k === "\n") {
      const it = items[msel];
      if (it === "—") return;
      if (it.cmd[0] === "INTERNAL:quit") { restore(); process.exit(0); }
      if (it.cmd[0] === "INTERNAL:rescan") { tiles = scan(); readTray(); query = ""; sel = 0; msel = 0; view = "home"; redraw(); return; }
      if (it.cmd[0] === "INTERNAL:files") { msel = 0; openFiles("/root"); redraw(); return; }
      if (it.cmd[0] === "INTERNAL:apps") { msel = 0; asel = 0; view = "apps"; redraw(); return; }
      if (it.cmd[0] === "INTERNAL:sys") { msel = 0; ysel = 0; readPs(); readPorts(); view = "sys"; redraw(); return; }
      if (it.cmd[0] === "INTERNAL:run") { msel = 0; inputBuf = ""; inputBuf2 = "/root"; rfocus = 0; view = "run"; redraw(); return; }
      if (it.cmd[0] === "INTERNAL:settings") { msel = 0; ssel = 0; view = "settings"; redraw(); return; }
      msel = 0; view = "home"; launch(it);
      return;
    }
    return;
  }
  // --- home (grid 2D ala GUI) ---
  if (k === "\x03" || k === "q") { restore(); process.exit(0); }
  if (k === "\x1b") { restore(); process.exit(0); }
  const list = filtered();
  const cols = gridCols();
  if (k === "\t") { msel = 0; view = "menu"; redraw(); return; }
  const moveH = (d) => {
    sel += d;
    if (sel < 0) sel = list.length - 1;
    if (sel >= list.length) sel = 0;
  };
  if (k === "\r" || k === "\n") {
    const cur = list[sel];
    if (!cur) return;
    if (cur.cmd[0] === "INTERNAL:files") { openFiles("/root"); redraw(); return; }
    if (cur.cmd[0] === "INTERNAL:apps") { asel = 0; view = "apps"; redraw(); return; }
    if (cur.cmd[0] === "INTERNAL:sys") { ysel = 0; readPs(); readPorts(); view = "sys"; redraw(); return; }
    if (cur.cmd[0] === "INTERNAL:run") { inputBuf = ""; inputBuf2 = "/root"; rfocus = 0; view = "run"; redraw(); return; }
    if (cur.cmd[0] === "INTERNAL:settings") { ssel = 0; view = "settings"; redraw(); return; }
    launch(cur);
    return;
  }
  if (k === "\x7f" || k === "\b") { query = query.slice(0, -1); sel = 0; redraw(); return; }
  if (k === "r" && !query) { tiles = scan(); readTray(); sel = 0; redraw(); return; }
  // navigasi home = PANAH saja (ala fzf): huruf selalu jadi filter.
  // Dulu hjkl ikut navigasi sehingga ketik "shell"/"jalankan" rusak
  // ('h'/'j' dimakan navigasi). Di view lain tanpa ketikan, hjkl tetap jalan.
  if (k.startsWith("\x1b[")) {
    if (k === "\x1b[A") sel = Math.max(0, sel - cols);
    if (k === "\x1b[B") sel = Math.min(list.length - 1, sel + cols);
    if (k === "\x1b[C") moveH(1);
    if (k === "\x1b[D") moveH(-1);
    redraw(); return;
  }
  if (/^[\x20-\x7e]$/.test(k)) { query += k; sel = 0; redraw(); return; }
  void escBuf;
};
process.stdin.on("data", (chunk) => {
  for (const k of keys(chunk)) handleKey(k);
});
