// --- 1. Universal Serial Engine Driver ---
    let port = null, reader = null, writer = null;
    let isConnected = false, isFirmwareVerified = false;
    let handshakeTimeout = null;
    let isDisconnecting = false;
    let sensitiveCommandEcho = null;
    let sensitiveCommandEchoTimer = null;

    const FIRMWARE_NAME = "Android TV ADB";
    const SIGNATURE_REGEX = /adb|android_tv|tv-|esp32/i;
    const PROBE_COMMANDS = ['', 'tv-status'];

    // Dynamic State
    let isTvConnected = null;
    let discoveredPackages = [];
    let isLoadingPackages = false;
    let packageListError = false;
    let isLoadingAppProcesses = false;
    let appProcessQueryError = false;
    let appProcessResponseReceived = false;
    let appProcessQueryTimeout = null;
    let appProcessRefreshPending = false;
    let appProcessNames = new Set();
    let appProcessStates = new Map();
    let loadedChannels = [];
    let discoveredWifiNetworks = new Map();
    let isScanningWifi = false;
    let esp32Status = 'Disconnected';
    let tvStatus = 'Unknown';
    let wifiStatus = 'Unknown';

    function renderStatusBar() {
      const statusText = document.getElementById('statusText');
      const statusTv = document.getElementById('statusTv');
      const statusWifi = document.getElementById('statusWifi');
      const statusDot = document.getElementById('statusDot');
      if (statusText) statusText.textContent = `ESP32: ${esp32Status}`;
      if (statusTv) statusTv.textContent = `TV: ${tvStatus}`;
      if (statusWifi) statusWifi.textContent = `Wi-Fi: ${wifiStatus}`;
      [[statusTv, tvStatus], [statusWifi, wifiStatus]].forEach(([element, value]) => {
        if (!element) return;
        element.classList.remove('status-good', 'status-error', 'status-pending');
        if (value === 'Connected') element.classList.add('status-good');
        else if (/failed|error/i.test(value)) element.classList.add('status-error');
        else if (/connecting|disconnecting|updating/i.test(value)) element.classList.add('status-pending');
      });
      if (statusDot) {
        statusDot.classList.remove('connected', 'error');
        if (esp32Status === 'Connected') statusDot.classList.add('connected');
        else if (esp32Status.toLowerCase().includes('error') || esp32Status.includes('Unrecognized')) statusDot.classList.add('error');
      }
      updateActionAvailability();
    }

    function setGlobalStatusMessage(message, tone = 'info') {
      const status = document.getElementById('statusMessage');
      if (!status) return;
      status.textContent = message;
      status.classList.remove('status-good', 'status-error', 'status-pending');
      if (tone === 'success') status.classList.add('status-good');
      else if (tone === 'error') status.classList.add('status-error');
      else if (tone === 'pending') status.classList.add('status-pending');
    }

    function clearAppProcessQueryTimeout() {
      if (appProcessQueryTimeout !== null) {
        clearTimeout(appProcessQueryTimeout);
        appProcessQueryTimeout = null;
      }
    }

    function updateActionAvailability() {
      const firmwareReady = isFirmwareVerified && writer !== null;
      const tvReady = firmwareReady && isTvConnected === true;
      ['btnWifiScan', 'btnWifiSetup', 'btnWifiForget', 'btnExportChannels', 'btnSendCmd']
        .forEach(id => {
          const button = document.getElementById(id);
          if (button) button.disabled = !firmwareReady;
        });
      const refreshAppsButton = document.getElementById('btnRefreshApps');
      if (refreshAppsButton) {
        refreshAppsButton.disabled = !tvReady
          || isLoadingPackages
          || isLoadingAppProcesses
          || appProcessRefreshPending;
      }
      const quickLaunchButton = document.getElementById('btnQuickLaunchTyped');
      if (quickLaunchButton) {
        quickLaunchButton.disabled = !tvReady
          || isLoadingPackages
          || isLoadingAppProcesses
          || appProcessRefreshPending;
      }
      const tvToggle = document.getElementById('btnTvToggleConnect');
      if (tvToggle) {
        tvToggle.disabled = !firmwareReady || tvStatus === 'Connecting' || tvStatus === 'Disconnecting';
      }
      document.querySelectorAll('[data-tv-cmd]').forEach(button => { button.disabled = !tvReady; });
      document.querySelectorAll('.app-toggle').forEach(button => {
        button.disabled = button.disabled || !tvReady;
      });
      document.querySelectorAll('.tune-channel').forEach(button => { button.disabled = !tvReady; });
      const networkPicker = document.getElementById('wifiNetworkPicker');
      if (networkPicker) networkPicker.disabled = !firmwareReady;
      const exportButton = document.getElementById('btnExportChannels');
      if (exportButton) exportButton.disabled = !tvReady;
      const downloadButton = document.getElementById('btnDownloadCsv');
      if (downloadButton) downloadButton.disabled = loadedChannels.length === 0;
    }

    function setConnectionState(state, info) {
      const btn = document.getElementById('btnConnect');
      const statusDot = document.getElementById('statusDot');
      const statusText = document.getElementById('statusText');
      
      if (btn) btn.classList.remove('connected', 'connecting', 'unrecognized');
      if (statusDot) statusDot.classList.remove('connected', 'error');

      if (state === 'connected') {
        isConnected = true;
        isFirmwareVerified = true;
        esp32Status = 'Connected';
        if (btn) {
          btn.classList.add('connected');
          btn.dataset.tooltip = `Connected: ${info || FIRMWARE_NAME} - Click to Disconnect`;
        }
        if (statusDot) statusDot.classList.add('connected');
        if (statusText) statusText.textContent = `ESP32: Connected`;
        setTimeout(() => {
          if (isFirmwareVerified) {
            sendSerialCmd('tv-status');
            sendSerialCmd('wifi status');
          }
        }, 100);
      } else if (state === 'probing') {
        clearAppProcessQueryTimeout();
        isConnected = true;
        isFirmwareVerified = false;
        esp32Status = 'Connecting';
        isTvConnected = null;
        tvStatus = 'Unknown';
        wifiStatus = 'Unknown';
        discoveredPackages = [];
        isLoadingPackages = false;
        packageListError = false;
        isLoadingAppProcesses = false;
        appProcessQueryError = false;
        appProcessResponseReceived = false;
        appProcessRefreshPending = false;
        appProcessNames.clear();
        appProcessStates.clear();
        sensitiveCommandEcho = null;
        if (sensitiveCommandEchoTimer) clearTimeout(sensitiveCommandEchoTimer);
        sensitiveCommandEchoTimer = null;
        discoveredWifiNetworks.clear();
        renderAppList([]);
        renderWifiDropdown();
        if (btn) {
          btn.classList.add('connecting');
          btn.dataset.tooltip = `Probing target device for ${FIRMWARE_NAME}...`;
        }
        if (statusText) statusText.textContent = 'ESP32: Connecting';
      } else if (state === 'unrecognized') {
        isConnected = true;
        isFirmwareVerified = false;
        esp32Status = 'Unrecognized firmware';
        if (btn) {
          btn.classList.add('unrecognized');
          btn.dataset.tooltip = 'Target did not respond with expected ADB signatures';
        }
        if (statusDot) statusDot.classList.add('error');
        if (statusText) statusText.textContent = 'ESP32: Unrecognized firmware';
      } else {
        clearAppProcessQueryTimeout();
        isConnected = false;
        isFirmwareVerified = false;
        esp32Status = 'Disconnected';
        isTvConnected = null;
        tvStatus = 'Unknown';
        wifiStatus = 'Unknown';
        discoveredPackages = [];
        isLoadingPackages = false;
        packageListError = false;
        isLoadingAppProcesses = false;
        appProcessQueryError = false;
        appProcessResponseReceived = false;
        appProcessRefreshPending = false;
        appProcessNames.clear();
        appProcessStates.clear();
        sensitiveCommandEcho = null;
        if (sensitiveCommandEchoTimer) clearTimeout(sensitiveCommandEchoTimer);
        sensitiveCommandEchoTimer = null;
        discoveredWifiNetworks.clear();
        renderAppList([]);
        renderWifiDropdown();
        if (btn) {
          btn.dataset.tooltip = 'Connect ESP32 (115200 Baud)';
        }
        if (statusText) statusText.textContent = 'ESP32: Disconnected';
      }
      renderStatusBar();
    }

    function logTerminal(msg, type = 'info') {
      const box = document.getElementById('termLog');
      if (!box) return;
      const el = document.createElement('div');
      el.className = `log-entry log-${type}`;
      el.textContent = msg;
      box.appendChild(el);
      box.scrollTop = box.scrollHeight;
    }

    async function sendSerialCmd(cmd, sensitive = false) {
      sensitive = sensitive || /^wifi setup\s+/.test(cmd);
      if (!writer) {
        logTerminal(`[ERR] Cannot send command; connect an ESP32 first.`, 'err');
        return false;
      }
      if (!isFirmwareVerified && cmd !== '' && cmd !== 'tv-status') {
        logTerminal('[ERR] Firmware is not verified; command was not sent.', 'err');
        return false;
      }
      if (/^tv-(?:key|volume|app-|export|shell|device-info)/.test(cmd) && isTvConnected !== true) {
        logTerminal('[ERR] Connect to an Android TV before sending this command.', 'err');
        return false;
      }
      try {
        if (sensitive) {
          sensitiveCommandEcho = cmd;
          if (sensitiveCommandEchoTimer) clearTimeout(sensitiveCommandEchoTimer);
          sensitiveCommandEchoTimer = setTimeout(() => { sensitiveCommandEcho = null; }, 5000);
        }
        const data = new TextEncoder().encode(cmd + '\r\n');
        await writer.write(data);
        if (cmd && !sensitive) logTerminal(`TX >> ${cmd}`, 'tx');
        else if (cmd) logTerminal('TX >> wifi setup [credentials hidden]', 'tx');
        return true;
      } catch (err) {
        if (sensitive) {
          sensitiveCommandEcho = null;
          if (sensitiveCommandEchoTimer) clearTimeout(sensitiveCommandEchoTimer);
          sensitiveCommandEchoTimer = null;
        }
        logTerminal(`[ERR] Write error: ${err.message}`, 'err');
        return false;
      }
    }

    function updateTvStatus(connected, host = '') {
      const wasTvConnected = isTvConnected;
      isTvConnected = connected;
      tvStatus = connected ? 'Connected' : 'Disconnected';
      const badge = document.getElementById('tvConnBadge');
      const toggleBtn = document.getElementById('btnTvToggleConnect');

      if (connected) {
        if (badge) {
          badge.textContent = 'CONNECTED';
          badge.style.color = 'var(--accent-emerald-soft)';
        }
        if (toggleBtn) {
          toggleBtn.classList.add('connected');
          toggleBtn.dataset.tooltip = `Disconnect from Android TV (${host || 'Online'})`;
        }
        if (!wasTvConnected && discoveredPackages.length === 0) {
          setTimeout(() => requestAppList(), 600);
        }
      } else {
        clearAppProcessQueryTimeout();
        isLoadingPackages = false;
        isLoadingAppProcesses = false;
        appProcessQueryError = false;
        appProcessResponseReceived = false;
        appProcessRefreshPending = false;
        appProcessNames.clear();
        appProcessStates.clear();
        if (badge) {
          badge.textContent = 'DISCONNECTED';
          badge.style.color = 'var(--text-muted)';
        }
        if (toggleBtn) {
          toggleBtn.classList.remove('connected');
          toggleBtn.dataset.tooltip = 'Connect to Android TV';
        }
        renderAppList(discoveredPackages);
      }
      renderStatusBar();
    }

    async function connectSerial() {
      if (isConnected) {
        await disconnectSerial();
        return;
      }
      if (!('serial' in navigator)) {
        alert('Web Serial API is not supported in this browser. Please open in Google Chrome or Microsoft Edge.');
        return;
      }
      try {
        port = await navigator.serial.requestPort();
        await port.open({ baudRate: 115200 });
        writer = port.writable.getWriter();

        setConnectionState('probing');
        toggleTerminalDrawer(true);
        logTerminal(`[SYS] Serial port opened at 115200 baud. Probing for ${FIRMWARE_NAME}...`, 'info');

        startReadLoop();

        let delay = 100;
        for (const cmd of PROBE_COMMANDS) {
          setTimeout(() => { if (!isFirmwareVerified && isConnected) sendSerialCmd(cmd); }, delay);
          delay += 250;
        }

        if (handshakeTimeout) clearTimeout(handshakeTimeout);
        handshakeTimeout = setTimeout(() => {
          if (isConnected && !isFirmwareVerified) {
            setConnectionState('unrecognized');
            logTerminal(`[WARN] Probe timeout: Target device did not respond with expected signatures.`, 'warn');
          }
        }, 3000);
      } catch (err) {
        logTerminal(`[ERR] Connection failed: ${err.message}`, 'err');
        await disconnectSerial();
        esp32Status = 'Connection error';
        renderStatusBar();
      }
    }

    async function disconnectSerial() {
      if (isDisconnecting) return;
      isDisconnecting = true;
      if (handshakeTimeout) { clearTimeout(handshakeTimeout); handshakeTimeout = null; }
      if (reader) {
        try { await reader.cancel(); }
        catch (err) { logTerminal(`[ERR] Serial reader cleanup failed: ${err.message}`, 'err'); }
        reader = null;
      }
      if (writer) {
        try { await writer.close(); }
        catch (err) { logTerminal(`[ERR] Serial writer cleanup failed: ${err.message}`, 'err'); }
        writer = null;
      }
      if (port) {
        try { await port.close(); }
        catch (err) { logTerminal(`[ERR] Serial port cleanup failed: ${err.message}`, 'err'); }
        port = null;
      }
      isDisconnecting = false;
      setConnectionState('disconnected');
      logTerminal('[SYS] Serial port disconnected.', 'info');
    }

    async function startReadLoop() {
      const textDecoder = new TextDecoderStream();
      port.readable.pipeTo(textDecoder.writable);
      reader = textDecoder.readable.getReader();
      let rxBuffer = '';
      try {
        while (true) {
          const { value, done } = await reader.read();
          if (done) {
            if (isConnected && !isDisconnecting) {
              logTerminal('[WARN] Serial connection closed by the device.', 'warn');
              await disconnectSerial();
            }
            break;
          }
          if (value) {
            if (typeof handleIncomingChunk === 'function') handleIncomingChunk(value);
            rxBuffer += value;
            const lines = rxBuffer.split(/\r?\n/);
            rxBuffer = lines.pop();
            for (const line of lines) {
              const tr = line.trim();
              if (!tr) continue;
              const displayLine = sensitiveCommandEcho && tr.includes(sensitiveCommandEcho)
                ? tr.replace(sensitiveCommandEcho, 'wifi setup [credentials hidden]')
                : tr;
              if (displayLine !== tr) {
                sensitiveCommandEcho = null;
                if (sensitiveCommandEchoTimer) clearTimeout(sensitiveCommandEchoTimer);
                sensitiveCommandEchoTimer = null;
              }
              logTerminal(displayLine, 'rx');
              handleIncomingLine(tr);
              if (sensitiveCommandEcho && (tr.includes('Wi-Fi credentials') || tr.startsWith('Error:'))) {
                sensitiveCommandEcho = null;
                if (sensitiveCommandEchoTimer) clearTimeout(sensitiveCommandEchoTimer);
                sensitiveCommandEchoTimer = null;
              }

              if (!isFirmwareVerified && isConnected && SIGNATURE_REGEX.test(tr)) {
                if (handshakeTimeout) { clearTimeout(handshakeTimeout); handshakeTimeout = null; }
                setConnectionState('connected', `${FIRMWARE_NAME} Online`);
                logTerminal(`[SYS] Firmware verified: ${FIRMWARE_NAME} Online.`, 'rx');
              }
            }
            if (rxBuffer.trim() === 'tv>') {
              logTerminal('tv>', 'rx');
              handleIncomingLine('tv>');
              rxBuffer = '';
            }
          }
        }
      } catch (e) {
        logTerminal(`[ERR] Serial read failed: ${e.message}`, 'err');
        if (isConnected && !isDisconnecting) await disconnectSerial();
      } finally {
        if (reader) try { reader.releaseLock(); } catch(e) {}
      }
    }

    function handleIncomingLine(line) {
      if (isLoadingAppProcesses && handleAppProcessLine(line)) return;

      if (line.startsWith('Error:') && isLoadingPackages) {
        isLoadingPackages = false;
        packageListError = true;
        appProcessRefreshPending = false;
        renderAppList([]);
        setGlobalStatusMessage('Could not read installed packages from the TV.', 'error');
        updateActionAvailability();
      }

      if (line.includes('Android TV ADB: connected') || line.includes('Android TV connected to')) {
        updateTvStatus(true);
      } else if (line.includes('Android TV ADB: disconnected') || line.includes('Android TV disconnected')) {
        updateTvStatus(false);
      } else if (/^Wi-Fi:\s*(not configured|connected|connecting|reconnect limit reached)/i.test(line)) {
        const match = line.match(/^Wi-Fi:\s*(.+)$/i);
        if (match) {
          const value = match[1].trim().toLowerCase();
          wifiStatus = value === 'connected' ? 'Connected' : value === 'not configured' ? 'Not configured' : value === 'connecting' ? 'Connecting' : 'Reconnect failed';
          renderStatusBar();
        }
      } else if (line.includes('Wi-Fi connected')) {
        wifiStatus = 'Connected';
        renderStatusBar();
      } else if (line.includes('Wi-Fi connection failed')) {
        wifiStatus = 'Failed';
        renderStatusBar();
      } else if (line.includes('Wi-Fi credentials saved') || line.includes('Reconnecting to Wi-Fi')) {
        wifiStatus = 'Connecting';
        renderStatusBar();
      } else if (line.includes('Wi-Fi credentials forgotten')) {
        wifiStatus = 'Not configured';
        renderStatusBar();
      } else if (line.includes('Wi-Fi reconnect limit reached')) {
        wifiStatus = 'Reconnect failed';
        renderStatusBar();
      } else if (line.includes('Wi-Fi ready:')) {
        wifiStatus = 'Connected';
        renderStatusBar();
      } else if (line.includes('Wi-Fi is not configured')) {
        wifiStatus = 'Not configured';
        renderStatusBar();
      } else if (line.startsWith('package:')) {
        const pkg = line.slice('package:'.length).trim();
        if (pkg && !discoveredPackages.includes(pkg)) {
          packageListError = false;
          discoveredPackages.push(pkg);
          appProcessStates.set(pkg, 'unknown');
          renderAppList(discoveredPackages);
        }
      } else if (line.trim() === 'tv>') {
        if (isLoadingPackages) {
          isLoadingPackages = false;
          if (packageListError) {
            renderAppList([]);
            updateActionAvailability();
          } else {
            requestAppProcessStatus();
          }
        } else if (isLoadingAppProcesses) {
          if (appProcessResponseReceived) completeAppProcessQuery();
        } else if (appProcessRefreshPending) {
          appProcessRefreshPending = false;
          requestAppList();
        }
      } else if (line.startsWith('Error:') && (tvStatus === 'Connecting' || tvStatus === 'Disconnecting')) {
        tvStatus = tvStatus === 'Connecting' ? 'Connection failed' : 'Disconnect failed';
        renderStatusBar();
      } else if (line.startsWith('Error:') && (wifiStatus === 'Connecting' || wifiStatus === 'Updating')) {
        wifiStatus = 'Connection failed';
        renderStatusBar();
      } else if (line.includes('TV channel export request failed') || line.includes('TV export transfer failed')) {
        setGlobalStatusMessage('TV channel export failed; see serial log.', 'error');
      } else if (line.startsWith('WIFI_AP:')) {
        // Parse: WIFI_AP: "SSID" RSSI: -54 AUTH: WPA2 CH: 1
        const apMatch = line.match(/^WIFI_AP:\s*"([^"]+)"\s*RSSI:\s*(-?\d+)\s*AUTH:\s*([^\s]+)(?:\s*CH:\s*(\d+))?/i);
        if (apMatch) {
          const ssid = apMatch[1];
          const rssi = parseInt(apMatch[2], 10);
          const auth = apMatch[3];
          const ch = apMatch[4] ? parseInt(apMatch[4], 10) : 1;
          discoveredWifiNetworks.set(ssid, { ssid, rssi, auth, ch });
          renderWifiDropdown();
        }

      } else if (line.includes('Wi-Fi scan failed')) {
        finishWifiScan(true);
      } else if (line.includes('Wi-Fi scan complete') || line.includes('No networks found')) {
        finishWifiScan(false);
      }
    }

    renderStatusBar();
