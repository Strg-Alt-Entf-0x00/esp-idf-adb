// --- 4. Remote Control Button Handling ---
    document.querySelectorAll('[data-tv-cmd]').forEach(btn => {
      btn.addEventListener('click', () => {
        const cmd = btn.dataset.tvCmd;
        if (cmd) sendSerialCmd(cmd);
      });
    });

    window.addEventListener('keydown', (e) => {
      if (['INPUT', 'SELECT', 'TEXTAREA'].includes(document.activeElement.tagName) || isTvConnected !== true) return;

      if (e.key === 'ArrowUp') { e.preventDefault(); sendSerialCmd('tv-key up'); }
      else if (e.key === 'ArrowDown') { e.preventDefault(); sendSerialCmd('tv-key down'); }
      else if (e.key === 'ArrowLeft') { e.preventDefault(); sendSerialCmd('tv-key left'); }
      else if (e.key === 'ArrowRight') { e.preventDefault(); sendSerialCmd('tv-key right'); }
      else if (e.key === 'Enter') { e.preventDefault(); sendSerialCmd('tv-key ok'); }
      else if (e.key === 'Escape' || e.key === 'Backspace') { e.preventDefault(); sendSerialCmd('tv-key back'); }
      else if (e.key.toLowerCase() === 'h') { e.preventDefault(); sendSerialCmd('tv-key home'); }
      else if (e.key.toLowerCase() === 'm') { e.preventDefault(); sendSerialCmd('tv-key menu'); }
      else if (e.key === ' ') { e.preventDefault(); sendSerialCmd('tv-key 126'); }
      else if (e.key === '+' || e.key === '=') { e.preventDefault(); sendSerialCmd('tv-volume up 1'); }
      else if (e.key === '-' || e.key === '_') { e.preventDefault(); sendSerialCmd('tv-volume down 1'); }
    });

    // --- 5. Channels Table & Export ---
    let channelTransfer = null;
    let channelTransferBuffer = '';

    function setChannelStatus(message, tone = 'info') {
      setGlobalStatusMessage(message, tone);
    }

    function updateServiceTypeFilter() {
      const filter = document.getElementById('chanServiceTypeFilter');
      if (!filter) return;

      const selectedType = filter.value;
      const serviceTypes = Array.from(new Set(
        loadedChannels.map(channel => channel.type.trim()).filter(Boolean)
      )).sort((a, b) => a.localeCompare(b));
      filter.innerHTML = [
        '<option value="">All service types</option>',
        ...serviceTypes.map(type =>
          `<option value="${escapeHtml(type)}">${escapeHtml(formatServiceType(type))}</option>`
        )
      ].join('');
      filter.value = serviceTypes.includes(selectedType) ? selectedType : '';
    }

    function formatServiceType(serviceType) {
      const value = String(serviceType || '').trim();
      if (value.toUpperCase() === 'SERVICE_TYPE_AUDIO_VIDEO') return 'Audio + video';
      const label = value.replace(/^SERVICE_TYPE_/i, '').replace(/_/g, ' ').toLowerCase();
      return label.replace(/\b[a-z]/g, character => character.toUpperCase()) || '—';
    }

    function parseCsvRows(csv) {
      const rows = [];
      let row = [], field = '', quoted = false;
      for (let i = 0; i < csv.length; i++) {
        const char = csv[i];
        if (char === '"') {
          if (quoted && csv[i + 1] === '"') { field += '"'; i++; }
          else quoted = !quoted;
        } else if (char === ',' && !quoted) {
          row.push(field); field = '';
        } else if ((char === '\n' || char === '\r') && !quoted) {
          if (char === '\r' && csv[i + 1] === '\n') i++;
          row.push(field); field = '';
          if (row.some(value => value !== '')) rows.push(row);
          row = [];
        } else {
          field += char;
        }
      }
      if (quoted) return null;
      if (field !== '' || row.length > 0) {
        row.push(field);
        if (row.some(value => value !== '')) rows.push(row);
      }
      return rows;
    }

    function handleIncomingChunk(chunk) {
      channelTransferBuffer += chunk;

      if (!channelTransfer) {
        const markerIndex = channelTransferBuffer.indexOf('TVCSV_BEGIN:');
        if (markerIndex >= 0) {
          channelTransferBuffer = channelTransferBuffer.slice(markerIndex);
        }
        const begin = channelTransferBuffer.match(/TVCSV_BEGIN:([a-f0-9]{32}):(\d+)\r+\n/i);
        if (!begin) {
          if (markerIndex >= 0) {
            if (channelTransferBuffer.length > 80) {
              channelTransferBuffer = '';
              setChannelStatus('The TV channel transfer header is invalid.', 'error');
            }
            return;
          }
          const markerLength = 'TVCSV_BEGIN:'.length;
          channelTransferBuffer = channelTransferBuffer.slice(-(markerLength - 1));
          return;
        }
        const expectedSize = Number(begin[2]);
        if (!Number.isSafeInteger(expectedSize) || expectedSize < 1 || expectedSize > 1024 * 1024) {
          channelTransferBuffer = '';
          setChannelStatus('The TV channel transfer size is invalid.', 'error');
          return;
        }
        channelTransfer = { requestId: begin[1], expectedSize };
        channelTransferBuffer = channelTransferBuffer.slice(begin.index + begin[0].length);
        setChannelStatus('Receiving the TV channel export…', 'pending');
      }

      if (channelTransferBuffer.length > channelTransfer.expectedSize * 2 + 256) {
        channelTransfer = null;
        channelTransferBuffer = '';
        setChannelStatus('The TV channel transfer exceeded its declared size.', 'error');
        return;
      }

      const abortMarker = `TVCSV_ABORT:${channelTransfer.requestId}`;
      if (channelTransferBuffer.includes(abortMarker)) {
        channelTransfer = null;
        channelTransferBuffer = '';
        loadedChannels = [];
        updateServiceTypeFilter();
        renderChannels(loadedChannels);
        setChannelStatus('The ESP32 could not complete the TV channel transfer.', 'error');
        return;
      }

      const endMarker = `TVCSV_END:${channelTransfer.requestId}`;
      const endIndex = channelTransferBuffer.indexOf(endMarker);
      if (endIndex < 0) return;

      const csv = channelTransferBuffer.slice(0, endIndex)
        .replace(/^(?:\r\r\n|\r?\n)/, '')
        .replace(/(?:\r\r\n|\r?\n)$/, '')
        .replace(/\r\r\n/g, '\r\n');
      const expectedSize = channelTransfer.expectedSize;
      channelTransferBuffer = channelTransferBuffer.slice(endIndex + endMarker.length);
      channelTransfer = null;

      if (new TextEncoder().encode(csv).length !== expectedSize) {
        loadedChannels = [];
        updateServiceTypeFilter();
        renderChannels(loadedChannels);
        setChannelStatus('The TV channel transfer size did not match the firmware report.', 'error');
        return;
      }

      const rows = parseCsvRows(csv);
      if (!rows || rows.length < 2) {
        loadedChannels = [];
        updateServiceTypeFilter();
        renderChannels(loadedChannels);
        setChannelStatus(
          rows ? 'No channel rows received.' : 'The TV returned malformed channel CSV data.',
          rows ? 'pending' : 'error'
        );
        return;
      }

      const header = rows[0].map(value => value.replace(/^\uFEFF/, '').trim());
      const channelIndex = header.indexOf('channel_number');
      const nameIndex = header.indexOf('channel_name');
      const typeIndex = header.indexOf('service_type');
      if (channelIndex < 0 || nameIndex < 0) {
        loadedChannels = [];
        updateServiceTypeFilter();
        renderChannels(loadedChannels);
        setChannelStatus('The TV returned an unsupported channel CSV format.', 'error');
        return;
      }

      loadedChannels = rows.slice(1).map(row => ({
        num: row[channelIndex] || '',
        name: row[nameIndex] || '',
        type: typeIndex >= 0 ? (row[typeIndex] || '') : ''
      })).filter(channel => channel.num || channel.name || channel.type);
      updateServiceTypeFilter();
      renderChannels(loadedChannels);
      setChannelStatus('TV channel list received and loaded.', 'success');
    }

    document.getElementById('btnExportChannels').addEventListener('click', () => {
      loadedChannels = [];
      updateServiceTypeFilter();
      renderChannels(loadedChannels);
      setChannelStatus('Requesting channel list from the TV…', 'pending');
      sendSerialCmd('tv-export').then(sent => {
        if (!sent) setChannelStatus('Could not request the TV channel export.', 'error');
      });
    });

    document.getElementById('btnDownloadCsv').addEventListener('click', () => {
      if (loadedChannels.length === 0) return;
      const csvCell = value => {
        const text = String(value ?? '');
        const safeText = /^[\t\r ]*[=+\-@]/.test(text) ? `'${text}` : text;
        return `"${safeText.replace(/"/g, '""')}"`;
      };
      const lines = [
        ['channel_number', 'channel_name', 'service_type'].map(csvCell).join(','),
        ...loadedChannels.map(channel => [channel.num, channel.name, channel.type].map(csvCell).join(','))
      ];
      const blob = new Blob([`\uFEFF${lines.join('\r\n')}\r\n`], { type: 'text/csv;charset=utf-8;' });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement('a');
      anchor.href = url;
      anchor.download = `tv-channels-${Date.now()}.csv`;
      anchor.click();
      URL.revokeObjectURL(url);
    });

    function tuneChannel(channelNumber) {
      if (!/^\d+$/.test(String(channelNumber)) || isTvConnected !== true) return;
      const digits = String(channelNumber).split('');
      let delay = 50;
      digits.forEach(digit => {
        const keyCode = 7 + Number(digit);
        setTimeout(() => sendSerialCmd(`tv-key ${keyCode}`), delay);
        delay += 120;
      });
      setTimeout(() => sendSerialCmd('tv-key ok'), delay + 100);
    }

    function renderChannels(channels) {
      const tbody = document.getElementById('chanTableBody');
      const count = document.getElementById('chanCountSuffix');
      if (count) count.textContent = `${channels.length} / ${loadedChannels.length}`;

      if (!tbody) return;
      if (channels.length === 0) {
        const emptyMessage = loadedChannels.length === 0
          ? 'No TV channels loaded. Export the channel list from the TV.'
          : 'No matching channels.';
        tbody.innerHTML = `<tr><td colspan="4" style="text-align: center; color: var(--text-muted); padding: 24px;">${emptyMessage}</td></tr>`;
        updateActionAvailability();
        return;
      }

      tbody.innerHTML = channels.map(channel => `
        <tr>
          <td style="font-family: var(--font-mono); font-weight: 700; color: #fff;">${escapeHtml(channel.num || '—')}</td>
          <td style="font-weight: 600;">${escapeHtml(channel.name || '—')}</td>
          <td style="color: var(--text-muted);" title="${escapeHtml(channel.type || '')}">${escapeHtml(formatServiceType(channel.type))}</td>
          <td style="text-align: right;">
            ${/^\d+$/.test(String(channel.num)) ? '<button type="button" class="btn btn-primary tune-channel" style="padding: 2px 8px; font-size: 10px;">Tune</button>' : ''}
            ${/^\d+$/.test(String(channel.num)) ? '' : '<span class="channel-not-tunable" title="The TV did not report a numeric channel number." aria-label="Not tunable: no numeric channel number">—</span>'}
          </td>
        </tr>
      `).join('');
      const tunableChannels = channels.filter(channel => /^\d+$/.test(String(channel.num)));
      tbody.querySelectorAll('.tune-channel').forEach((button, index) => {
        button.disabled = isTvConnected !== true;
        button.addEventListener('click', () => tuneChannel(tunableChannels[index].num));
      });
      updateActionAvailability();
    }

    function filterChannels() {
      const query = document.getElementById('chanSearchInput').value.toLowerCase().trim();
      const serviceType = document.getElementById('chanServiceTypeFilter').value;
      const filtered = loadedChannels.filter(channel =>
        (!query
          || (channel.name && channel.name.toLowerCase().includes(query))
          || (channel.num && String(channel.num).toLowerCase().includes(query)))
        && (!serviceType || channel.type === serviceType)
      );
      renderChannels(filtered);
    }

    updateServiceTypeFilter();
    renderChannels(loadedChannels);
