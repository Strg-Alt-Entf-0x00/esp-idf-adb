// --- 3. Dynamic App List Management ---
function getVisiblePackages() {
  const query = document.getElementById('appSearchInput').value.toLowerCase().trim();
  return query
    ? discoveredPackages.filter(pkg => pkg.toLowerCase().includes(query))
    : discoveredPackages;
}

function renderAppList(apps) {
  const container = document.getElementById('appListContainer');
  if (!apps || apps.length === 0) {
    const message = isLoadingPackages
      ? 'Loading app packages from the TV…'
      : packageListError
        ? 'Could not load app packages. Check the TV connection and retry.'
        : discoveredPackages.length > 0
          ? 'No matching packages.'
          : 'No app packages were reported by the TV. Refresh to query again.';
    container.innerHTML = `
      <div style="text-align: center; color: var(--text-muted); font-size: 11px; padding: 30px;">
        ${message}
      </div>`;
    updateActionAvailability();
    return;
  }

  container.innerHTML = apps.map(pkg => {
    const escapedPackage = escapeHtml(pkg);
    const state = appProcessQueryError
      ? 'unknown'
      : (appProcessStates.get(pkg) || (isLoadingAppProcesses ? 'checking' : 'unknown'));
    const stateLabel = state === 'running'
      ? 'Running'
      : state === 'stopped'
        ? 'Not running'
        : state === 'checking'
          ? 'Checking device state…'
          : 'State unavailable';
    const actionLabel = state === 'running' ? 'Stop' : state === 'stopped' ? 'Start' : 'Checking…';
    const actionTitle = state === 'running'
      ? `Force-stop ${pkg}`
      : state === 'stopped'
        ? `Attempt to launch ${pkg}`
        : `Waiting for a confirmed process state for ${pkg}`;
    const disabled = !isFirmwareVerified
      || isTvConnected !== true
      || isLoadingPackages
      || isLoadingAppProcesses
      || appProcessRefreshPending
      || !['running', 'stopped'].includes(state);
    return `
      <div class="app-list-row">
        <div class="app-meta">
          <div class="app-avatar">${escapeHtml(pkg.charAt(0).toUpperCase())}</div>
          <div class="app-info-col">
            <span class="app-title-text">${escapedPackage}</span>
            <span class="app-state ${state}">${stateLabel}</span>
          </div>
        </div>
        <div class="app-actions-col">
          <button type="button" class="btn btn-secondary app-toggle" data-package="${escapedPackage}" data-state="${state}" title="${escapeHtml(actionTitle)}" aria-label="${actionLabel} ${escapedPackage}"${disabled ? ' disabled' : ''}>
            ${actionLabel}
          </button>
        </div>
      </div>
    `;
  }).join('');

  container.querySelectorAll('.app-toggle').forEach(button => {
    button.addEventListener('click', () => toggleAppPackage(button.dataset.package));
  });
  updateActionAvailability();
}

function renderVisibleApps() {
  renderAppList(getVisiblePackages());
}

function requestAppList() {
  clearAppProcessQueryTimeout();
  discoveredPackages = [];
  appProcessStates.clear();
  appProcessNames.clear();
  isLoadingPackages = true;
  packageListError = false;
  isLoadingAppProcesses = false;
  appProcessQueryError = false;
  appProcessRefreshPending = false;
  renderAppList([]);
  setGlobalStatusMessage('Reading installed packages from the TV…', 'pending');
  updateActionAvailability();
  sendSerialCmd('tv-app-list').then(sent => {
    if (!sent) {
      isLoadingPackages = false;
      packageListError = true;
      renderAppList([]);
      setGlobalStatusMessage('Could not read installed packages from the TV.', 'error');
    }
  });
}

function requestAppProcessStatus() {
  clearAppProcessQueryTimeout();
  appProcessNames.clear();
  appProcessQueryError = false;
  appProcessResponseReceived = false;
  isLoadingAppProcesses = true;
  discoveredPackages.forEach(pkg => appProcessStates.set(pkg, 'checking'));
  renderVisibleApps();
  setGlobalStatusMessage('Checking app processes reported by the TV…', 'pending');
  updateActionAvailability();
  appProcessQueryTimeout = setTimeout(() => {
    appProcessQueryTimeout = null;
    if (isLoadingAppProcesses) {
      appProcessQueryError = true;
      completeAppProcessQuery();
    }
  }, 10000);
  sendSerialCmd('tv-shell ps -A -o NAME').then(sent => {
    if (!sent) {
      clearAppProcessQueryTimeout();
      isLoadingAppProcesses = false;
      appProcessQueryError = true;
      appProcessResponseReceived = false;
      discoveredPackages.forEach(pkg => appProcessStates.set(pkg, 'unknown'));
      renderVisibleApps();
      setGlobalStatusMessage('Could not read app process state; actions are disabled.', 'error');
      updateActionAvailability();
    }
  });
}

function handleAppProcessLine(line) {
  const value = line.trim();
  if (value === 'tv>' || /^(?:Wi-Fi(?::|\s)|Android TV|WIFI_AP:)/i.test(value)) return false;
  if (!value) return true;
  if (/^(?:tv>\s*)?tv-shell\s+ps\s+-A\s+-o\s+NAME$/i.test(value)) {
    appProcessResponseReceived = true;
    return true;
  }
  if (/^(?:NAME|CMD|COMMAND|CMDLINE)$/i.test(value)) {
    appProcessResponseReceived = true;
    return true;
  }
  if (/^(?:Error:|ps:\s*(?:bad|unknown|usage|error)|usage:\s*ps|unknown.*(?:option|field))/i.test(value)) {
    appProcessQueryError = true;
    appProcessResponseReceived = true;
    return true;
  }

  const processName = value.split(/\s+/).pop();
  if (/^[A-Za-z0-9._:-]+$/.test(processName) || /^\[[A-Za-z0-9._:-]+\]$/.test(processName)) {
    appProcessNames.add(processName);
  } else {
    appProcessQueryError = true;
  }
  appProcessResponseReceived = true;
  return true;
}

function completeAppProcessQuery() {
  clearAppProcessQueryTimeout();
  isLoadingAppProcesses = false;
  appProcessResponseReceived = false;
  if (appProcessNames.size === 0) appProcessQueryError = true;

  if (appProcessQueryError) {
    discoveredPackages.forEach(pkg => appProcessStates.set(pkg, 'unknown'));
    setGlobalStatusMessage('App process state unavailable; start/stop actions are disabled.', 'error');
  } else {
    discoveredPackages.forEach(pkg => {
      const isRunning = Array.from(appProcessNames).some(name =>
        name === pkg || name.startsWith(`${pkg}:`)
      );
      appProcessStates.set(pkg, isRunning ? 'running' : 'stopped');
    });
    setGlobalStatusMessage('App process state refreshed from the TV.', 'success');
  }

  renderVisibleApps();
  updateActionAvailability();
}

function toggleAppPackage(packageName) {
  const state = appProcessStates.get(packageName);
  if (!['running', 'stopped'].includes(state)
      || isTvConnected !== true
      || isLoadingPackages
      || isLoadingAppProcesses
      || appProcessRefreshPending) return;

  const command = state === 'running' ? 'tv-app-stop' : 'tv-app-launch';
  const nextStatus = state === 'running' ? 'stopping' : 'starting';
  appProcessStates.set(packageName, 'checking');
  appProcessRefreshPending = true;
  renderVisibleApps();
  setGlobalStatusMessage(`${nextStatus === 'starting' ? 'Start' : 'Stop'} requested; verifying device state…`, 'pending');
  sendSerialCmd(`${command} ${packageName}`).then(sent => {
    if (!sent) {
      appProcessRefreshPending = false;
      appProcessStates.set(packageName, state);
      renderVisibleApps();
      setGlobalStatusMessage(`Could not send ${command} for ${packageName}.`, 'error');
    }
  });
}

function filterApps() {
  renderVisibleApps();
}

function launchTypedPackage() {
  const value = document.getElementById('appSearchInput').value.trim();
  if (!value) return;
  if (!/^[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+$/.test(value)) {
    alert('Enter a valid Android package name.');
    return;
  }
  appProcessRefreshPending = true;
  updateActionAvailability();
  sendSerialCmd(`tv-app-launch ${value}`).then(sent => {
    if (!sent) {
      appProcessRefreshPending = false;
      updateActionAvailability();
    }
  });
}

document.getElementById('btnRefreshApps').addEventListener('click', requestAppList);
