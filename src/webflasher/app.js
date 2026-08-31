import { ESPLoader, Transport } from "https://cdn.jsdelivr.net/npm/esptool-js@0.6.1/bundle.js";

const MONITOR_BAUD = 115200;

const SLOTS = [
  { name: "bootloader",       addr: 0x0,     url: "firmware/bootloader.bin" },
  { name: "partition-table",  addr: 0x8000,  url: "firmware/partition-table.bin" },
  { name: "wyze-hijack_ap",   addr: 0x10000, url: "firmware/wyze-hijack_ap.bin" },
];

let device = null, transport = null, esploader = null, chip = null;
let monitoring = false, monReader = null;

const term = document.getElementById("term");
const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function tw(s){ term.textContent += s; term.scrollTop = term.scrollHeight; }
function status(s){ $("status").textContent = s; }

const espTerminal = {
  clean(){ term.textContent = ""; },
  writeLine(d){ tw(d + "\n"); },
  write(d){ tw(d); },
};

function renderSlots(){
  const host = $("slots");
  host.innerHTML = "";
  SLOTS.forEach((sl, i) => {
    const row = document.createElement("div");
    row.className = "slot";
    row.innerHTML =
      '<span class="addr">0x' + sl.addr.toString(16) + '</span>' +
      '<span>' + sl.name + '</span>' +
      '<span class="st ' + (sl.data ? "ok" : "miss") + '" id="st' + i + '">' +
        (sl.data ? "bundled (" + sl.data.length + " B)" : "missing") + '</span>';
    host.appendChild(row);
  });
}

async function loadBundled(){
  for (const sl of SLOTS){
    try {
      const r = await fetch(sl.url, { cache: "no-store" });
      if (!r.ok) throw 0;
      sl.data = new Uint8Array(await r.arrayBuffer());
    } catch (e) { sl.data = null; }
  }
  renderSlots();
  const have = SLOTS.filter((s) => s.data).length;
  status(have === SLOTS.length ? "Bundled firmware loaded." :
    "Bundled firmware not reachable (" + have + "/" + SLOTS.length + "). Serve this folder over http://localhost.");
}

window.connect = async function(){
  if (!("serial" in navigator)) { status("Web Serial unavailable — use desktop Chrome/Edge."); return; }
  try {
    device = await navigator.serial.requestPort();
    transport = new Transport(device, false);
    esploader = new ESPLoader({ transport, baudrate: parseInt($("flashBaud").value), terminal: espTerminal });
    chip = await esploader.main();
    $("chip").textContent = chip;
    const s3 = /S3/i.test(chip);
    const pill = $("chipPill");
    pill.textContent = s3 ? "ESP32-S3 OK" : chip + " (expected S3!)";
    pill.className = "pill " + (s3 ? "ok" : "");
    $("btnFlash").disabled = false;
    $("btnConnect").disabled = true;
    $("btnDisconnect").disabled = false;
    status("Connected.");
  } catch (e) { status("Connect failed: " + e.message); }
};

window.flash = async function(){
  try {
    const fileArray = [];
    for (let i = 0; i < SLOTS.length; i++){
      const data = SLOTS[i].data;
      if (!data) { status("Missing bundled firmware: " + SLOTS[i].name + ". Serve over http://localhost so it can load."); return; }
      fileArray.push({ data, address: SLOTS[i].addr });
    }
    $("btnFlash").disabled = true; $("btnMon").disabled = true;
    $("prog").value = 0;
    const grand = fileArray.reduce((a, f) => a + f.data.length, 0);
    const done = [];
    await esploader.writeFlash({
      fileArray,
      flashSize: "keep", flashMode: "keep", flashFreq: "keep",
      eraseAll: $("eraseChk").checked,
      compress: $("compressChk").checked,
      reportProgress: (idx, written) => {
        done[idx] = written;
        const sum = done.reduce((a, b) => a + (b || 0), 0);
        $("prog").value = Math.round((sum / grand) * 100);
      },
    });
    status("Flash complete. Start the monitor to view output.");
  } catch (e) {
    status("Flash failed: " + e.message);
  } finally {
    $("btnFlash").disabled = false; $("btnMon").disabled = false;
  }
};

async function pulseReset(){
  await device.setSignals({ dataTerminalReady: false, requestToSend: true });
  await sleep(120);
  await device.setSignals({ requestToSend: false });
}

window.resetTarget = async function(){
  if (!device) { status("Not connected."); return; }
  const opened = !device.readable;
  try {
    if (opened) await device.open({ baudRate: MONITOR_BAUD });
    await pulseReset();
    if (opened) await device.close();
    status("Target reset.");
  } catch (e) { status("Reset failed: " + e.message); }
};

window.toggleMonitor = async function(){
  if (monitoring) { await stopMonitor(); return; }
  if (!("serial" in navigator)) { status("Web Serial unavailable — use desktop Chrome/Edge."); return; }
  try { if (!device) device = await navigator.serial.requestPort(); }
  catch (e) { status("No port: " + e.message); return; }
  if (transport) { try { await transport.disconnect(); } catch (e) {} }
  try {
    if (device.readable) { try { await device.close(); } catch (e) {} }
    await device.open({ baudRate: MONITOR_BAUD });
  } catch (e) { status("Monitor open failed: " + e.message); return; }
  monitoring = true;
  $("btnMon").textContent = "Stop monitor";
  status("Monitoring at " + MONITOR_BAUD + " baud.");
  (async () => {
    const dec = new TextDecoder();
    try {
      while (monitoring && device.readable) {
        monReader = device.readable.getReader();
        try {
          while (true) {
            const { value, done } = await monReader.read();
            if (done) break;
            if (value) tw(dec.decode(value));
          }
        } finally {
          try { monReader.releaseLock(); } catch (e) {}
          monReader = null;
        }
      }
    } catch (e) { if (monitoring) tw("\n[monitor error: " + e.message + "]\n"); }
  })();
  try { await pulseReset(); } catch (e) {}
};

async function stopMonitor(){
  if (!monitoring) return;
  monitoring = false;
  try { if (monReader) await monReader.cancel(); } catch (e) {}
  try { await device.close(); } catch (e) {}
  $("btnMon").textContent = "Start monitor";
  status("Monitor stopped.");
}

window.disconnectAll = async function(){
  await stopMonitor();
  if (transport) { try { await transport.disconnect(); } catch (e) {} }
  try { if (device && device.readable) await device.close(); } catch (e) {}
  transport = null; esploader = null; chip = null; device = null;
  $("chip").textContent = "-";
  $("chipPill").textContent = "not connected"; $("chipPill").className = "pill";
  $("btnConnect").disabled = false; $("btnDisconnect").disabled = true; $("btnFlash").disabled = true;
  status("Disconnected.");
};

renderSlots();
loadBundled();
