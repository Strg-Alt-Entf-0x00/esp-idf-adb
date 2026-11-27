// --- 6. Tabs, Drawer & Help Modal ---
    function switchTab(tabId, activeButton) {
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-body').forEach(t => t.style.display = 'none');
      document.getElementById(tabId).style.display = 'flex';
      if (activeButton) activeButton.classList.add('active');
    }

    const termDrawer = document.getElementById('bottomTerminalDrawer');
    const btnToggleTerm = document.getElementById('btnToggleTerm');

    function toggleTerminalDrawer(forceState) {
      const isOpen = forceState !== undefined ? forceState : !termDrawer.classList.contains('open');
      termDrawer.classList.toggle('open', isOpen);
      btnToggleTerm.classList.toggle('active', isOpen);
    }

    btnToggleTerm.addEventListener('click', () => toggleTerminalDrawer());
    document.getElementById('btnCloseDrawer').addEventListener('click', () => toggleTerminalDrawer(false));
    document.getElementById('btnClearTerm').addEventListener('click', () => {
      document.getElementById('termLog').innerHTML = '';
    });
    document.getElementById('btnCopyTerm').addEventListener('click', async () => {
      try {
        await copyTextToClipboard(readTerminalLog());
      } catch (err) {
        logTerminal(`Could not copy serial log: ${err.message}`, 'err');
      }
    });
    document.getElementById('btnDownloadTerm').addEventListener('click', downloadTerminalLog);

    function readTerminalLog() {
      const box = document.getElementById('termLog');
      return Array.from(box.children, line => line.textContent).join('\n');
    }

    async function copyTextToClipboard(text) {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(text);
        return;
      }

      const textarea = document.createElement('textarea');
      textarea.value = text;
      textarea.setAttribute('readonly', '');
      textarea.style.position = 'fixed';
      textarea.style.opacity = '0';
      document.body.appendChild(textarea);
      let copied;
      try {
        textarea.select();
        copied = document.execCommand('copy');
      } finally {
        textarea.remove();
      }
      if (!copied) throw new Error('Clipboard access is unavailable in this browser.');
    }

    function downloadTerminalLog() {
      let objectUrl;
      try {
        const blob = new Blob([readTerminalLog()], { type: 'text/plain;charset=utf-8' });
        objectUrl = URL.createObjectURL(blob);
        const link = document.createElement('a');
        link.href = objectUrl;
        link.download = 'adb-serial-console-log.txt';
        document.body.appendChild(link);
        link.click();
        link.remove();
      } catch (err) {
        logTerminal(`Could not download serial log: ${err.message}`, 'err');
      } finally {
        if (objectUrl) window.setTimeout(() => URL.revokeObjectURL(objectUrl), 1000);
      }
    }

    document.getElementById('btnConnect').addEventListener('click', connectSerial);
    document.getElementById('btnSendCmd').addEventListener('click', () => {
      const input = document.getElementById('termCmdInput');
      if (input.value.trim()) { sendSerialCmd(input.value.trim()); input.value = ''; }
    });
    document.getElementById('termCmdInput').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        const input = document.getElementById('termCmdInput');
        if (input.value.trim()) { sendSerialCmd(input.value.trim()); input.value = ''; }
      }
    });

    const helpModal = document.getElementById('helpModal');
    document.getElementById('btnHelp').addEventListener('click', () => helpModal.classList.add('open'));
    document.getElementById('btnCloseHelp').addEventListener('click', () => helpModal.classList.remove('open'));
    helpModal.addEventListener('click', (e) => { if (e.target === helpModal) helpModal.classList.remove('open'); });
