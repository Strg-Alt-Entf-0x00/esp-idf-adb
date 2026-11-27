// --- 2. TV Connection & Toggle Button ---
    let wifiScanTimeout = null;

    document.getElementById('btnTvToggleConnect').addEventListener('click', () => {
      if (isTvConnected) {
        const previousStatus = tvStatus;
        tvStatus = 'Disconnecting';
        renderStatusBar();
        sendSerialCmd('tv-disconnect').then((sent) => {
          if (!sent) { tvStatus = previousStatus; renderStatusBar(); }
        });
      } else {
        const host = document.getElementById('tvHost').value.trim();
        const portVal = document.getElementById('tvPort').value.trim() || '5555';
        if (!/^(?:[A-Za-z0-9.-]+|\[[0-9A-Fa-f:.]+\])$/.test(host)) {
          alert('Enter a valid Android TV IP address or host name.');
          return;
        }
        if (!/^\d+$/.test(portVal) || Number(portVal) < 1 || Number(portVal) > 65535) {
          alert('Enter a valid port between 1 and 65535.');
          return;
        }
        const previousStatus = tvStatus;
        tvStatus = 'Connecting';
        renderStatusBar();
        sendSerialCmd(`tv-connect ${host} ${portVal}`).then((sent) => {
          if (!sent) { tvStatus = previousStatus; renderStatusBar(); }
        });
      }
    });

    // --- Wi-Fi Scanner & Management ---
    function escapeHtml(str) {
      if (!str) return '';
      return String(str)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
    }

    function scanWifiNetworks() {
      if (isScanningWifi) return;
      isScanningWifi = true;
      const btn = document.getElementById('btnWifiScan');
      if (btn) btn.classList.add('spinning');
      const picker = document.getElementById('wifiNetworkPicker');
      if (picker) picker.innerHTML = '<option value="">Scanning…</option>';
      setGlobalStatusMessage('Scanning for nearby Wi-Fi networks…', 'pending');
      logTerminal('[WIFI] Requesting active scan (wifi scan)...', 'info');

      discoveredWifiNetworks.clear();
      if (!writer || !isFirmwareVerified) {
        isScanningWifi = false;
        if (btn) btn.classList.remove('spinning');
        if (picker) picker.innerHTML = '<option value="">—</option>';
        setGlobalStatusMessage('Connect the ESP32 before scanning for Wi-Fi networks.', 'error');
        return;
      }
      sendSerialCmd('wifi scan');

      wifiScanTimeout = setTimeout(() => {
        wifiScanTimeout = null;
        if (isScanningWifi) {
          finishWifiScan(true);
        }
      }, 8000);
    }

    function finishWifiScan(failed = false) {
      isScanningWifi = false;
      if (wifiScanTimeout !== null) {
        clearTimeout(wifiScanTimeout);
        wifiScanTimeout = null;
      }
      const btn = document.getElementById('btnWifiScan');
      if (btn) btn.classList.remove('spinning');
      if (failed) {
        const picker = document.getElementById('wifiNetworkPicker');
        if (picker) picker.innerHTML = '<option value="">—</option>';
        setGlobalStatusMessage('Wi-Fi scan failed; see serial log.', 'error');
      } else {
        renderWifiDropdown();
        const networkCount = discoveredWifiNetworks.size;
        const networkLabel = networkCount === 1 ? 'network' : 'networks';
        setGlobalStatusMessage(
          networkCount
            ? `Wi-Fi scan complete: ${networkCount} ${networkLabel} found.`
            : 'Wi-Fi scan complete: no networks found.',
          networkCount ? 'success' : 'info'
        );
      }
    }

    function renderWifiDropdown() {
      const picker = document.getElementById('wifiNetworkPicker');
      if (!picker) return;

      const networks = Array.from(discoveredWifiNetworks.values())
        .sort((a, b) => b.rssi - a.rssi);

      if (networks.length === 0) {
        picker.innerHTML = '<option value="">—</option>';
        return;
      }

      let html = '<option value="">—</option>';
      networks.forEach(ap => {
        const lockIcon = (ap.auth === 'OPEN') ? '🔓' : '🔒';
        let signalBars = '▂▄▆█';
        if (ap.rssi < -80) signalBars = '▂___';
        else if (ap.rssi < -70) signalBars = '▂▄__';
        else if (ap.rssi < -60) signalBars = '▂▄▆_';

        html += `<option value="${escapeHtml(ap.ssid)}">
          ${escapeHtml(ap.ssid)} (${ap.rssi} dBm ${signalBars} ${lockIcon} ${ap.auth})
        </option>`;
      });
      picker.innerHTML = html;
    }

    function onSelectDiscoveredNetwork(ssid) {
      if (!ssid) return;
      const ap = discoveredWifiNetworks.get(ssid);
      const ssidInput = document.getElementById('wifiSsid');
      const passInput = document.getElementById('wifiPass');

      if (ssidInput) ssidInput.value = ssid;
      if (ap) {
        if (ap.auth === 'OPEN') {
          if (passInput) {
            passInput.value = '';
            passInput.placeholder = 'Open network (no password required)';
          }
        } else {
          if (passInput) {
            passInput.placeholder = 'Enter password for ' + ssid;
            passInput.focus();
          }
        }
      } else {
        if (passInput) passInput.focus();
      }
      logTerminal(`[WIFI] Selected network "${ssid}". Ready for connect.`, 'info');
    }

    document.getElementById('btnWifiSetup').addEventListener('click', () => {
      const ssid = document.getElementById('wifiSsid').value;
      const pass = document.getElementById('wifiPass').value;
      if (!ssid.trim()) { alert('Please enter Wi-Fi SSID'); return; }
      const quoteArgument = (value) => `"${value.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
      const previousStatus = wifiStatus;
      wifiStatus = 'Connecting';
      renderStatusBar();
      sendSerialCmd(`wifi setup ${quoteArgument(ssid)} ${quoteArgument(pass)}`, true).then((sent) => {
        if (sent) {
          setTimeout(() => sendSerialCmd('wifi connect'), 300);
        } else {
          wifiStatus = previousStatus;
          renderStatusBar();
        }
      });
    });

    document.getElementById('btnWifiForget').addEventListener('click', () => {
      const previousStatus = wifiStatus;
      wifiStatus = 'Updating';
      renderStatusBar();
      sendSerialCmd('wifi forget').then((sent) => {
        if (!sent) {
          wifiStatus = previousStatus;
          renderStatusBar();
        }
      });
    });
