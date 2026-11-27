import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import vm from "node:vm";

const appScript = [
  "serial-simulator.js",
  "connection-wifi.js",
  "app-library.js",
  "remote-channels.js",
  "ui.js"
].map((file) => readFileSync(new URL(`../tools-web/js/${file}`, import.meta.url), "utf8")).join("\n");
const appHtml = readFileSync(new URL("../tools-web/index.html", import.meta.url), "utf8");

class FakeElement {
  constructor(fragment = false) {
    this.isFragment = fragment;
    this.listeners = new Map();
    this.children = [];
    this.dataset = {};
    this.style = {};
    this.value = "";
    this.textContent = "";
    this.innerHTML = "";
    this.scrollHeight = 0;
    this.scrollTop = 0;
    this.classes = new Set();
    this.querySelectorCache = new Map();
    this.classList = {
      add: (name) => this.classes.add(name),
      remove: (name) => this.classes.delete(name),
      toggle: (name, force) => {
        const shouldAdd = force ?? !this.classes.has(name);
        if (shouldAdd) this.classes.add(name);
        else this.classes.delete(name);
        return shouldAdd;
      }
    };
  }
  get className() { return Array.from(this.classes).join(" "); }
  addEventListener(type, callback) {
    const callbacks = this.listeners.get(type) ?? [];
    callbacks.push(callback);
    this.listeners.set(type, callbacks);
  }
  async dispatch(type, event = {}) {
    const callbacks = this.listeners.get(type) ?? [];
    await Promise.all(callbacks.map((callback) => callback(event)));
  }
  append(...children) { this.children.push(...children); }
  appendChild(child) { this.children.push(child); }
  replaceChildren(...children) { this.children = children; }
  setAttribute() {}
  querySelector() { return null; }
  querySelectorAll(selector) {
    if (selector !== ".app-toggle") return [];
    const cached = this.querySelectorCache.get(selector);
    if (cached?.markup === this.innerHTML) return cached.elements;

    const elements = Array.from(this.innerHTML.matchAll(/<button\b([^>]*)>([\s\S]*?)<\/button>/g))
      .filter(([, attributes]) => /\bclass="[^"]*\bapp-toggle\b/.test(attributes))
      .map(([, attributes, content]) => {
        const element = new FakeElement();
        element.dataset.package = attributes.match(/\bdata-package="([^"]*)"/)?.[1] ?? "";
        element.disabled = /\sdisabled(?:\s|$)/.test(attributes);
        element.textContent = content.replace(/<[^>]*>/g, "").trim();
        return element;
      });
    this.querySelectorCache.set(selector, { markup: this.innerHTML, elements });
    return elements;
  }
  focus() {}
  click() {}
}

function createApp() {
  const elements = new Map();
  const windowListeners = new Map();
  const document = {
    body: new FakeElement(),
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new FakeElement());
      return elements.get(id);
    },
    querySelector() { return new FakeElement(); },
    querySelectorAll() { return []; },
    createElement() { return new FakeElement(); },
    createTextNode(text) { return { textContent: text }; },
    createDocumentFragment() { return new FakeElement(true); },
    activeElement: { tagName: "BODY" }
  };
  const state = { controller: null, writes: [] };
  const port = {
    open: async () => {},
    close: async () => {},
    readable: new ReadableStream({
      start(controller) { state.controller = controller; }
    }),
    writable: new WritableStream({
      write(chunk) { state.writes.push(new Uint8Array(chunk)); }
    })
  };
  const serial = {
    addEventListener() {},
    requestPort: async () => port
  };
  class FakeTextDecoderStream {
    constructor() {
      const decoder = new TextDecoder();
      let controller;
      this.readable = new ReadableStream({ start(streamController) { controller = streamController; } });
      this.writable = new WritableStream({
        write(chunk) { controller.enqueue(decoder.decode(chunk, { stream: true })); }
      });
    }
  }
  const context = {
    document,
    navigator: { language: "en-US", serial },
    window: {
      confirm: () => true,
      setTimeout,
      clearTimeout,
      addEventListener(type, callback) {
        const callbacks = windowListeners.get(type) ?? [];
        callbacks.push(callback);
        windowListeners.set(type, callbacks);
      }
    },
    TextEncoder,
    TextDecoder,
    TextDecoderStream: FakeTextDecoderStream,
    Uint8Array,
    Intl,
    alert: () => {},
    setTimeout,
    clearTimeout
  };
  vm.runInNewContext(appScript, context, { filename: "tools-web/index.html" });
  return { elements, state, document, windowListeners, context };
}

async function connectAndStartExport(app) {
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 150));
  await app.elements.get("btnExportChannels").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(app.elements.get("statusMessage").textContent, "Requesting channel list from the TV…");
  const lastWrite = new TextDecoder().decode(app.state.writes.at(-1));
  assert.equal(lastWrite, "tv-export\r\n");
}

function makeFrame(csv, requestId) {
  const encoder = new TextEncoder();
  const csvBytes = encoder.encode(csv);
  return encoder.encode(
    `tv> tv-export\r\nTVCSV_BEGIN:${requestId}:${csvBytes.length}\r\n${csv}\r\nTVCSV_END:${requestId}\r\n`
  );
}

test("opens Wi-Fi setup first, followed by Android TV", () => {
  const tabStrip = appHtml.match(/<div class="tab-strip">([\s\S]*?)<\/div>/)?.[1];
  assert.ok(tabStrip);
  assert.ok(tabStrip.indexOf("Wi-Fi Setup") < tabStrip.indexOf("Android TV"));
  assert.match(tabStrip, /class="tab-btn active" onclick="switchTab\('tabWifi', this\)">Wi-Fi Setup/);
  assert.match(appHtml, /<div id="tabWifi" class="tab-body">/);
  assert.match(appHtml, /<div id="tabTarget" class="tab-body" style="display: none;">/);
});

test("imports a UTF-8 CSV received in arbitrarily split serial chunks", async () => {
  const app = createApp();
  await connectAndStartExport(app);
  const csv = "\uFEFF\"channel_number\",\"channel_name\",\"input_id\",\"service_type\"\r\n\"1\",\"News, HD\",\"tuner\",\"AUDIO_VIDEO\"\r\n\"2\",\"München\",\"tuner\",\"TYPE_DVB_S\"\r\n\"\",\"Netflix\",\"app-input\",\"SERVICE_TYPE_AUDIO_VIDEO\"\r\n";
  const frame = makeFrame(csv, "a".repeat(32));
  for (let offset = 0; offset < frame.length; offset += 7) {
    app.state.controller.enqueue(frame.slice(offset, offset + 7));
  }
  await new Promise((resolve) => setTimeout(resolve, 200));
  assert.equal(
    app.elements.get("chanCountSuffix").textContent,
    "3 / 3",
    `${app.elements.get("statusMessage").textContent}\n${app.elements.get("termLog").children.map(entry => entry.textContent).join("\n")}`
  );
  assert.equal(app.elements.get("statusMessage").textContent, "TV channel list received and loaded.");
  assert.ok(appHtml.includes('class="status-center" id="statusMessage"'));
  assert.equal(appHtml.includes('id="serialStatus"'), false);
  assert.ok(app.elements.get("chanServiceTypeFilter").innerHTML.includes("Audio + video"));
  assert.ok(app.elements.get("chanTableBody").innerHTML.includes("Not tunable"));

  app.elements.set("chanSearchInput", new FakeElement());
  app.elements.get("chanSearchInput").value = "münch";
  app.context.filterChannels();
  assert.equal(app.elements.get("chanCountSuffix").textContent, "1 / 3");
  assert.ok(app.elements.get("chanTableBody").innerHTML.includes("München"));

  app.elements.get("chanSearchInput").value = "";
  app.elements.get("chanServiceTypeFilter").value = "TYPE_DVB_S";
  app.context.filterChannels();
  assert.equal(app.elements.get("chanCountSuffix").textContent, "1 / 3");
  assert.ok(app.elements.get("chanTableBody").innerHTML.includes("München"));
  assert.equal(app.elements.get("chanTableBody").innerHTML.includes("Netflix"), false);
});

test("does not import a serial transfer aborted by the firmware", async () => {
  const app = createApp();
  await connectAndStartExport(app);
  const frame = new TextEncoder().encode(
    `TVCSV_BEGIN:${"b".repeat(32)}:100\r\npartial\r\nTVCSV_ABORT:${"b".repeat(32)}\r\n`
  );
  app.state.controller.enqueue(frame);
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusMessage").textContent, "The ESP32 could not complete the TV channel transfer.");
});

test("supports the CRLF-doubled serial framing variant", async () => {
  const app = createApp();
  await connectAndStartExport(app);
  const csv = "\uFEFF\"channel_number\",\"channel_name\",\"input_id\",\"service_type\"\r\n\"42\",\"Test Channel\",\"tuner\",\"TYPE_DVB_S\"\r\n";
  const frame = new TextEncoder().encode(
    `tv> tv-export\r\r\nTVCSV_BEGIN:${"c".repeat(32)}:${new TextEncoder().encode(csv).length}\r\r\n${csv.replace(/\r\n/g, "\r\r\n")}\r\r\nTVCSV_END:${"c".repeat(32)}\r\r\n`
  );
  app.state.controller.enqueue(frame);
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusMessage").textContent, "TV channel list received and loaded.");
  assert.equal(app.elements.get("chanCountSuffix").textContent, "1 / 1");
});

test("reports TV and Wi-Fi state in the global status bar", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusTv").textContent, "TV: Connected");

  await new Promise((resolve) => setTimeout(resolve, 120));
  const sentCommands = app.state.writes.map(bytes => new TextDecoder().decode(bytes));
  assert.ok(sentCommands.includes("wifi status\r\n"));
  assert.ok(sentCommands.includes("tv-status\r\n"));

  app.state.controller.enqueue(new TextEncoder().encode("Wi-Fi: connected\r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusWifi").textContent, "Wi-Fi: Connected");

  app.state.controller.enqueue(new TextEncoder().encode("Wi-Fi: reconnect limit reached\r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusWifi").textContent, "Wi-Fi: Reconnect failed");
});

test("reports Wi-Fi scan results in the global status bar", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: disconnected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 150));

  app.context.scanWifiNetworks();
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(new TextDecoder().decode(app.state.writes.at(-1)), "wifi scan\r\n");
  app.state.controller.enqueue(new TextEncoder().encode(
    'WIFI_AP: "Office Network" RSSI: -54 AUTH: WPA2 CH: 1\r\nWi-Fi scan complete\r\n'
  ));
  await new Promise((resolve) => setTimeout(resolve, 20));

  assert.equal(app.elements.get("statusMessage").textContent, "Wi-Fi scan complete: 1 network found.");
  assert.match(app.elements.get("wifiNetworkPicker").innerHTML, /Office Network/);
  assert.equal(appHtml.includes("wifiScanCountBadge"), false);
  assert.equal(appHtml.includes("1-Click Auto-Fill"), false);
  assert.equal(appHtml.includes("Manual / Selected"), false);

  app.context.scanWifiNetworks();
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("No networks found\r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusMessage").textContent, "Wi-Fi scan complete: no networks found.");

  app.context.scanWifiNetworks();
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Wi-Fi scan failed: ESP_FAIL\r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusMessage").textContent, "Wi-Fi scan failed; see serial log.");
});

test("uses real TV connect and disconnect responses without a manual Wi-Fi query button", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: disconnected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 150));
  assert.equal(appHtml.includes('id="btnWifiStatus"'), false);
  app.elements.set("tvHost", new FakeElement());
  app.elements.set("tvPort", new FakeElement());
  app.elements.get("tvHost").value = "192.168.178.21";
  app.elements.get("tvPort").value = "5555";
  await app.elements.get("btnTvToggleConnect").dispatch("click");
  assert.equal(app.elements.get("statusTv").textContent, "TV: Connecting");
  assert.equal(new TextDecoder().decode(app.state.writes.at(-1)), "tv-connect 192.168.178.21 5555\r\n");
  app.state.controller.enqueue(new TextEncoder().encode("Android TV connected to 192.168.178.21:5555.\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusTv").textContent, "TV: Connected");
  await app.elements.get("btnTvToggleConnect").dispatch("click");
  assert.equal(app.elements.get("statusTv").textContent, "TV: Disconnecting");
  app.state.controller.enqueue(new TextEncoder().encode("Android TV disconnected.\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusTv").textContent, "TV: Disconnected");
});

test("shows only packages returned by the TV", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));

  await app.elements.get("btnRefreshApps").dispatch("click");
  assert.equal(new TextDecoder().decode(app.state.writes.at(-1)), "tv-app-list\r\n");
  app.state.controller.enqueue(new TextEncoder().encode(
    "tv> tv-app-list\r\npackage:com.example.livingroom\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 20));
  app.state.controller.enqueue(new TextEncoder().encode(
    "tv-shell ps -A -o NAME\r\nNAME\r\nsystem_server\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 20));

  const appMarkup = app.elements.get("appListContainer").innerHTML;
  assert.ok(appMarkup.includes("com.example.livingroom"));
  assert.equal(appMarkup.includes("YouTube"), false);
  assert.equal(appMarkup.includes("Running"), false);
});

test("reports app-list command failures instead of claiming no packages were found", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  await app.elements.get("btnRefreshApps").dispatch("click");
  app.state.controller.enqueue(new TextEncoder().encode("Error: ESP_FAIL\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));

  const appMessage = app.elements.get("appListContainer").innerHTML;
  assert.ok(appMessage.includes("Could not load app packages"));
  assert.equal(appMessage.includes("No app packages were reported"), false);
});

test("app action toggles from process state and rechecks after force-stop", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 120));

  await app.elements.get("btnRefreshApps").dispatch("click");
  app.state.controller.enqueue(new TextEncoder().encode(
    "tv> tv-app-list\r\npackage:com.example.livingroom\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(new TextDecoder().decode(app.state.writes.at(-1)), "tv-shell ps -A -o NAME\r\n");

  app.state.controller.enqueue(new TextEncoder().encode("tv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(app.elements.get("statusMessage").textContent, "Checking app processes reported by the TV…");

  app.state.controller.enqueue(new TextEncoder().encode(
    "tv-shell ps -A -o NAME\r\nNAME\r\nsystem_server\r\ncom.example.livingroom\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 20));
  let action = app.elements.get("appListContainer").querySelectorAll(".app-toggle")[0];
  assert.equal(action.textContent, "Stop");
  assert.equal(action.disabled, false);
  assert.equal(app.elements.get("appListContainer").querySelectorAll(".app-toggle").length, 1);

  await action.dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(new TextDecoder().decode(app.state.writes.at(-1)), "tv-app-stop com.example.livingroom\r\n");
  app.state.controller.enqueue(new TextEncoder().encode(
    "tv> tv-app-stop com.example.livingroom\r\nThe ADB shell stream cannot confirm whether Android stopped the app.\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(new TextDecoder().decode(app.state.writes.at(-1)), "tv-app-list\r\n");

  app.state.controller.enqueue(new TextEncoder().encode(
    "tv> tv-app-list\r\npackage:com.example.livingroom\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode(
    "tv> tv-shell ps -A -o NAME\r\nNAME\r\nsystem_server\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 20));

  action = app.elements.get("appListContainer").querySelectorAll(".app-toggle")[0];
  assert.equal(action.textContent, "Start");
  assert.equal(action.disabled, false);
});

test("keeps app controls disabled when the device cannot report process state", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 120));

  await app.elements.get("btnRefreshApps").dispatch("click");
  app.state.controller.enqueue(new TextEncoder().encode(
    "tv> tv-app-list\r\npackage:com.example.livingroom\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode(
    "tv> tv-shell ps -A -o NAME\r\nps: bad -o argument 'NAME'\r\ntv> \r\n"
  ));
  await new Promise((resolve) => setTimeout(resolve, 20));

  const action = app.elements.get("appListContainer").querySelectorAll(".app-toggle")[0];
  assert.equal(action.textContent, "Checking…");
  assert.equal(action.disabled, true);
  assert.ok(app.elements.get("statusMessage").textContent.includes("process state unavailable"));
});

test("remote keyboard sends keys only while the TV is connected and focus is outside inputs", async () => {
  const app = createApp();
  const keydown = app.windowListeners.get("keydown")[0];
  const sendKey = async (key) => {
    let prevented = false;
    keydown({ key, preventDefault() { prevented = true; } });
    await new Promise((resolve) => setTimeout(resolve, 0));
    return prevented;
  };

  assert.equal(await sendKey("ArrowUp"), false);
  assert.equal(app.state.writes.length, 0);

  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 120));
  const writesBeforeKey = app.state.writes.length;
  assert.equal(await sendKey("ArrowUp"), true);
  assert.equal(new TextDecoder().decode(app.state.writes[writesBeforeKey]), "tv-key up\r\n");

  app.document.activeElement = { tagName: "INPUT" };
  const writesBeforeInputKey = app.state.writes.length;
  assert.equal(await sendKey("ArrowDown"), false);
  assert.equal(app.state.writes.length, writesBeforeInputKey);

  app.document.activeElement = { tagName: "BODY" };
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: disconnected\r\n"));
  await new Promise((resolve) => setTimeout(resolve, 10));
  const writesBeforeDisconnectedKey = app.state.writes.length;
  assert.equal(await sendKey("ArrowLeft"), false);
  assert.equal(app.state.writes.length, writesBeforeDisconnectedKey);
});

test("does not show seeded channels or demo device data", () => {
  const app = createApp();
  assert.equal(app.elements.get("chanCountSuffix").textContent, "0 / 0");
  assert.equal(app.elements.get("statusWifi").textContent, "Wi-Fi: Unknown");
  assert.equal(app.elements.get("btnWifiSetup").disabled, true);
  assert.equal(app.elements.get("btnTvToggleConnect").disabled, true);
  assert.equal(app.elements.get("btnDownloadCsv").disabled, true);
  assert.equal(appHtml.includes('value="192.168.1.150"'), false);
  assert.equal(appScript.includes("Sony"), false);
  assert.equal(appScript.includes("Das Erste HD"), false);
  assert.equal(appScript.includes("FRITZ!Box 7590"), false);
});

test("sends Wi-Fi credentials quoted and redacts the console echo", async () => {
  const app = createApp();
  await app.elements.get("btnConnect").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 10));
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: disconnected\r\ntv> \r\n"));
  await new Promise((resolve) => setTimeout(resolve, 150));

  app.elements.set("wifiSsid", new FakeElement());
  app.elements.set("wifiPass", new FakeElement());
  app.elements.get("wifiSsid").value = "Office Network";
  app.elements.get("wifiPass").value = "secret phrase";
  await app.elements.get("btnWifiSetup").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(
    new TextDecoder().decode(app.state.writes.at(-1)),
    'wifi setup "Office Network" "secret phrase"\r\n'
  );
  app.state.controller.enqueue(new TextEncoder().encode(
    'tv> wifi setup "Office Network" "secret phrase"\r\nWi-Fi credentials saved to NVS; connecting.\r\n'
  ));
  await new Promise((resolve) => setTimeout(resolve, 20));
  const terminalText = app.elements.get("termLog").children.map(entry => entry.textContent).join("\n");
  assert.equal(terminalText.includes("secret phrase"), false);
  assert.ok(terminalText.includes("wifi setup [credentials hidden]"));

  await new Promise((resolve) => setTimeout(resolve, 320));
  assert.equal(new TextDecoder().decode(app.state.writes.at(-1)), "wifi connect\r\n");
  app.state.controller.enqueue(new TextEncoder().encode("Wi-Fi connected.\r\n"));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("statusWifi").textContent, "Wi-Fi: Connected");
});

test("does not simulate successful commands while the ESP32 is disconnected", async () => {
  const app = createApp();
  await app.elements.get("btnWifiForget").dispatch("click");
  assert.equal(app.elements.get("statusWifi").textContent, "Wi-Fi: Unknown");
  assert.ok(app.elements.get("termLog").children.some(entry =>
    entry.textContent.includes("Cannot send command; connect an ESP32 first.")
  ));
});
