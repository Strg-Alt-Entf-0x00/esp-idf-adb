import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import vm from "node:vm";

const html = readFileSync(new URL("../tools-web/tv_channel_manager.html", import.meta.url), "utf8");
const inlineScript = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(inlineScript, "expected the web app to contain one inline script");

class FakeElement {
  constructor(fragment = false) {
    this.isFragment = fragment;
    this.listeners = new Map();
    this.children = [];
    this.dataset = {};
    this.style = {};
    this.value = "";
    this.textContent = "";
    this.scrollHeight = 0;
    this.scrollTop = 0;
    this.classes = new Set();
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

  addEventListener(type, callback) {
    const callbacks = this.listeners.get(type) ?? [];
    callbacks.push(callback);
    this.listeners.set(type, callbacks);
  }

  async dispatch(type, event = {}) {
    const callbacks = this.listeners.get(type) ?? [];
    return Promise.all(callbacks.map((callback) => callback(event)));
  }

  append(...children) {
    this.children.push(...children);
  }

  replaceChildren(...children) {
    this.children = children.flatMap((child) =>
      child?.isFragment ? child.children : [child]
    );
  }

  setAttribute() {}
  querySelector() { return null; }
  querySelectorAll() { return []; }
}

function createApp() {
  const elements = new Map();
  const document = {
    body: new FakeElement(),
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new FakeElement());
      return elements.get(id);
    },
    querySelector() {
      return new FakeElement();
    },
    createElement() {
      return new FakeElement();
    },
    createTextNode(text) {
      return { textContent: text };
    },
    createDocumentFragment() {
      return new FakeElement(true);
    }
  };
  const state = { controller: null, writes: [] };
  const port = {
    open: async () => {},
    close: async () => {},
    readable: new ReadableStream({
      start(controller) {
        state.controller = controller;
      }
    }),
    writable: new WritableStream({
      write(chunk) {
        state.writes.push(new Uint8Array(chunk));
      }
    })
  };
  const serial = {
    addEventListener() {},
    requestPort: async () => port
  };
  const context = {
    document,
    navigator: { language: "en-US", serial },
    window: { confirm: () => true, setTimeout, clearTimeout },
    TextEncoder,
    TextDecoder,
    Uint8Array,
    Intl
  };
  vm.runInNewContext(inlineScript, context, { filename: "tools-web/tv_channel_manager.html" });
  return { elements, state };
}

async function connectAndStartExport(app) {
  await app.elements.get("serialConnectButton").dispatch("click");
  await app.elements.get("serialExportButton").dispatch("click");
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(app.elements.get("serialStatus").textContent, "Waiting for the ESP32 to prepare the TV channel export…");
  assert.equal(
    new TextDecoder().decode(app.state.writes[0]),
    "tv-export\r\n"
  );
}

function makeFrame(csv, requestId) {
  const encoder = new TextEncoder();
  const csvBytes = encoder.encode(csv);
  return encoder.encode(
    `tv> tv-export\r\nTVCSV_BEGIN:${requestId}:${csvBytes.length}\r\n${csv}\r\nTVCSV_END:${requestId}\r\n`
  );
}

test("imports a UTF-8 CSV received in arbitrarily split serial chunks", async () => {
  const app = createApp();
  await connectAndStartExport(app);

  const csv = [
    "\uFEFF\"channel_number\",\"channel_name\",\"input_id\",\"broadcast_type\",\"service_type\",\"broadcast_visibility_type\",\"browsable\",\"locked\"\r\n",
    "\"1\",\"News, HD\",\"tuner\",\"TYPE_DVB_S\",\"AUDIO_VIDEO\",\"VISIBLE\",\"true\",\"false\"\r\n",
    "\"2\",\"München\",\"tuner\",\"TYPE_DVB_S\",\"AUDIO_VIDEO\",\"VISIBLE\",\"true\",\"false\"\r\n"
  ].join("");
  const frame = makeFrame(csv, "a".repeat(32));
  for (let offset = 0; offset < frame.length; offset += 7) {
    app.state.controller.enqueue(frame.slice(offset, offset + 7));
  }
  await new Promise((resolve) => setTimeout(resolve, 20));

  assert.equal(app.elements.get("tableCount").textContent, "2 of 2 channel rows");
  assert.equal(app.elements.get("tableBody").children.length, 2);
  assert.equal(app.elements.get("tableBody").children[0].children[2].children[0].textContent, "News, HD");
  assert.equal(app.elements.get("tableBody").children[1].children[2].children[0].textContent, "München");
  assert.equal(app.elements.get("serialStatus").textContent, "TV channel list received and loaded.");
});

test("does not import a serial transfer that the firmware aborts", async () => {
  const app = createApp();
  await connectAndStartExport(app);

  const requestId = "b".repeat(32);
  const frame = new TextEncoder().encode(
    `TVCSV_BEGIN:${requestId}:100\r\npartial\r\nTVCSV_ABORT:${requestId}\r\n`
  );
  app.state.controller.enqueue(frame);
  await new Promise((resolve) => setTimeout(resolve, 20));

  assert.equal(app.elements.get("tableBody").children.length, 0);
  assert.equal(app.elements.get("serialStatus").textContent, "The ESP32 could not complete the TV channel transfer.");
});

test("imports a CSV with CRLF-doubled serial framing (newlib VFS UART compatibility)", async () => {
  const app = createApp();
  await connectAndStartExport(app);

  const csv = [
    "\uFEFF\"channel_number\",\"channel_name\",\"input_id\",\"broadcast_type\",\"service_type\",\"broadcast_visibility_type\",\"browsable\",\"locked\"\r\n",
    "\"42\",\"Test Channel\",\"tuner\",\"TYPE_DVB_S\",\"AUDIO_VIDEO\",\"VISIBLE\",\"true\",\"false\"\r\n"
  ].join("");
  const encoder = new TextEncoder();
  const csvBytes = encoder.encode(csv);
  const requestId = "c".repeat(32);
  const frame = encoder.encode(
    `tv> tv-export\r\r\nTVCSV_BEGIN:${requestId}:${csvBytes.length}\r\r\n${csv}\r\r\nTVCSV_END:${requestId}\r\r\n`
  );

  for (let offset = 0; offset < frame.length; offset += 5) {
    app.state.controller.enqueue(frame.slice(offset, offset + 5));
  }
  await new Promise((resolve) => setTimeout(resolve, 20));

  assert.equal(app.elements.get("tableCount").textContent, "1 of 1 channel rows");
  assert.equal(app.elements.get("tableBody").children.length, 1);
  assert.equal(app.elements.get("tableBody").children[0].children[2].children[0].textContent, "Test Channel");
  assert.equal(app.elements.get("serialStatus").textContent, "TV channel list received and loaded.");
});

test("supports remote navigation commands and parses app listing", async () => {
  const app = createApp();
  await app.elements.get("serialConnectButton").dispatch("click");

  // Send TV connect
  app.elements.get("tvHostInput").value = "192.168.178.21";
  await app.elements.get("tvConnectButton").dispatch("click");
  assert.equal(
    new TextDecoder().decode(app.state.writes[app.state.writes.length - 1]),
    "tv-connect 192.168.178.21\r\n"
  );

  // Trigger app query
  await app.elements.get("tvFetchAppsButton").dispatch("click");
  assert.equal(
    new TextDecoder().decode(app.state.writes[app.state.writes.length - 1]),
    "tv-app-list\r\n"
  );

  // Feed simulated package list output
  const pkgOutput = new TextEncoder().encode(
    "package:com.google.android.youtube.tv\r\npackage:com.tcl.tv\r\n\r\ntv>         "
  );
  app.state.controller.enqueue(pkgOutput);
  await new Promise((resolve) => setTimeout(resolve, 20));

  // Verify dropdown populated
  const select = app.elements.get("tvAppSelect");
  assert.equal(select.children.length, 3); // default + 2 packages
  assert.equal(select.children[1].value, "com.google.android.youtube.tv");
  assert.equal(select.children[2].value, "com.tcl.tv");

  // Launch app
  select.value = "com.google.android.youtube.tv";
  await app.elements.get("tvLaunchAppButton").dispatch("click");
  assert.equal(
    new TextDecoder().decode(app.state.writes[app.state.writes.length - 1]),
    "tv-app-launch com.google.android.youtube.tv\r\n"
  );

  // Stop app
  await app.elements.get("tvStopAppButton").dispatch("click");
  assert.equal(
    new TextDecoder().decode(app.state.writes[app.state.writes.length - 1]),
    "tv-app-stop com.google.android.youtube.tv\r\n"
  );
});

test("toggles single connect/disconnect button state and switches workspace tabs", async () => {
  const app = createApp();
  const connectBtn = app.elements.get("serialConnectButton");

  assert.equal(connectBtn.textContent, "Connect to ESP32");
  await connectBtn.dispatch("click");

  // Connected state
  assert.equal(connectBtn.textContent, "Disconnect");
  assert.equal(app.elements.get("connectionBadge").className.includes("connecting"), true);

  // Feed prompt to verify handshake
  app.state.controller.enqueue(new TextEncoder().encode("Android TV ADB: connected\r\ntv> "));
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.elements.get("connectionBadge").className.includes("connected"), true);

  // Click disconnect on same button
  await connectBtn.dispatch("click");
  assert.equal(connectBtn.textContent, "Connect to ESP32");
  assert.equal(app.elements.get("connectionBadge").className.includes("disconnected"), true);
});
