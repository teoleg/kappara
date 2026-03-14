/* ── Kappara Trading Dashboard ── */
const WS_URL   = '/ws';
const MAX_PNL  = 60;  // PnL chart history points

let stompClient = null;
let pnlHistory  = [];
let priceChart  = null;
let pnlChart    = null;
let priceData   = {};  // symbol -> last 30 prices

// ── Connection ──────────────────────────────────────────
function connect() {
  const socket = new SockJS(WS_URL);
  stompClient  = new StompJs.Client({ webSocketFactory: () => socket });

  stompClient.onConnect = () => {
    setConnected(true);
    stompClient.subscribe('/topic/quotes',     m => onQuotes(JSON.parse(m.body)));
    stompClient.subscribe('/topic/positions',  m => onPositions(JSON.parse(m.body)));
    stompClient.subscribe('/topic/risk',       m => onRisk(JSON.parse(m.body)));
    stompClient.subscribe('/topic/trades',     m => onTrade(JSON.parse(m.body)));
    stompClient.subscribe('/topic/algorithms', m => onAlgorithms(JSON.parse(m.body)));
  };
  stompClient.onDisconnect = () => setConnected(false);
  stompClient.activate();
}

function setConnected(ok) {
  const el = document.getElementById('conn-status');
  el.textContent = ok ? 'LIVE' : 'DISCONNECTED';
  el.className   = 'badge ' + (ok ? 'badge-green' : 'badge-red');
}

// ── Quotes ───────────────────────────────────────────────
function onQuotes(quotes) {
  const tbody = document.getElementById('quotes-body');
  tbody.innerHTML = '';
  quotes.sort((a, b) => a.symbol.localeCompare(b.symbol));
  quotes.forEach(q => {
    if (!priceData[q.symbol]) priceData[q.symbol] = [];
    priceData[q.symbol].push(q.price);
    if (priceData[q.symbol].length > 30) priceData[q.symbol].shift();

    const chg    = q.change || 0;
    const chgPct = q.changePercent || 0;
    const cls    = chg >= 0 ? 'pos' : 'neg';
    const sign   = chg >= 0 ? '+' : '';

    tbody.insertAdjacentHTML('beforeend', `
      <tr>
        <td>${q.symbol}</td>
        <td class="${cls}">${fmt(q.price)}</td>
        <td>${fmt(q.bid)}</td>
        <td>${fmt(q.ask)}</td>
        <td class="${cls}">${sign}${fmt(chg)}</td>
        <td class="${cls}">${sign}${chgPct.toFixed(2)}%</td>
      </tr>`);
  });
  updatePriceChart(quotes);
}

// ── Positions ─────────────────────────────────────────────
function onPositions(positions) {
  const tbody = document.getElementById('positions-body');
  tbody.innerHTML = '';
  if (!positions.length) {
    tbody.insertAdjacentHTML('beforeend', '<tr><td colspan="8" style="color:var(--muted);text-align:center">No open positions</td></tr>');
    return;
  }
  positions.forEach(p => {
    const cls = p.unrealizedPnL >= 0 ? 'pos' : 'neg';
    tbody.insertAdjacentHTML('beforeend', `
      <tr>
        <td>${p.symbol}</td>
        <td class="${p.quantity > 0 ? 'pos' : 'neg'}">${p.quantity}</td>
        <td>${fmt(p.avgCost)}</td>
        <td>${fmt(p.currentPrice)}</td>
        <td>${fmtK(p.marketValue)}</td>
        <td class="${cls}">${fmtPnL(p.unrealizedPnL)}</td>
        <td class="${p.deltaExposure >= 0 ? 'pos' : 'neg'}">${fmtK(p.deltaExposure)}</td>
        <td style="color:var(--accent);font-size:10px">${shortStrategy(p.strategy)}</td>
      </tr>`);
  });
}

// ── Risk ──────────────────────────────────────────────────
function onRisk(r) {
  document.getElementById('nav').textContent   = fmtK(r.portfolioNAV);
  document.getElementById('upnl').textContent  = fmtPnL(r.totalUnrealizedPnL);
  document.getElementById('rpnl').textContent  = fmtPnL(r.totalRealizedPnL);
  document.getElementById('var').textContent   = fmtK(r.valueAtRisk);
  document.getElementById('delta').textContent = fmtK(r.netDelta);

  styleValue('upnl',  r.totalUnrealizedPnL);
  styleValue('rpnl',  r.totalRealizedPnL);
  styleValue('delta', r.netDelta);

  const statusEl = document.getElementById('risk-status');
  const cls = { GREEN: 'badge-green', YELLOW: 'badge-yellow', RED: 'badge-red' }[r.riskStatus] || 'badge-red';
  statusEl.innerHTML = `<span class="badge ${cls}">${r.riskStatus}</span>`;

  // Bars
  const varPct   = Math.min(r.varPercent / 4 * 100, 100);
  const deltaBar = Math.min(Math.abs(r.netDelta) / 100000 * 100, 100);
  const varColor = r.varPercent < 2 ? 'var(--green)' : r.varPercent < 3 ? 'var(--yellow)' : 'var(--red)';
  const dColor   = Math.abs(r.netDelta) < 50000 ? 'var(--green)' : 'var(--red)';

  setBar('var-bar',   varPct,   varColor);
  setBar('delta-bar', deltaBar, dColor);

  document.getElementById('var-pct-val').textContent = r.varPercent.toFixed(2) + '%';
  document.getElementById('delta-val').textContent   = fmtK(r.netDelta);
  document.getElementById('gross-exp').textContent   = fmtK(r.grossExposure);
  document.getElementById('net-exp').textContent     = fmtK(r.netExposure);

  // PnL chart
  pnlHistory.push(r.totalUnrealizedPnL + r.totalRealizedPnL);
  if (pnlHistory.length > MAX_PNL) pnlHistory.shift();
  updatePnlChart();
}

// ── Trades ────────────────────────────────────────────────
let tradeCount = 0;
function onTrade(trade) {
  const tbody = document.getElementById('trades-body');
  const cls   = trade.side === 'BUY' ? 'pos' : 'neg';
  const time  = new Date(trade.timestamp).toLocaleTimeString();
  tbody.insertAdjacentHTML('afterbegin', `
    <tr>
      <td style="color:var(--muted)">${time}</td>
      <td style="color:var(--muted)">${trade.tradeId}</td>
      <td>${trade.symbol}</td>
      <td class="${cls}">${trade.side}</td>
      <td>${trade.quantity}</td>
      <td>${fmt(trade.price)}</td>
      <td style="color:var(--accent);font-size:10px">${shortStrategy(trade.strategy)}</td>
      <td style="color:var(--muted);font-size:10px;max-width:180px;overflow:hidden;text-overflow:ellipsis">${trade.reason}</td>
    </tr>`);
  // Keep last 40 rows
  const rows = tbody.querySelectorAll('tr');
  if (rows.length > 40) rows[rows.length - 1].remove();
}

// ── Algorithms ────────────────────────────────────────────
function onAlgorithms(algos) {
  const container = document.getElementById('algo-list');
  container.innerHTML = '';
  algos.forEach(a => {
    if (!a) return;
    const decisionCls = decisionClass(a.lastDecision);
    const signal      = (a.signal || 0);
    const pct         = Math.round((signal + 1) / 2 * 100);
    const sigColor    = signal > 0 ? 'var(--green)' : signal < 0 ? 'var(--red)' : 'var(--muted)';
    const sigLeft     = signal >= 0 ? 50 : pct;
    const sigWidth    = Math.abs(pct - 50);

    let metricsHtml = '';
    if (a.metrics) {
      for (const [k, v] of Object.entries(a.metrics)) {
        metricsHtml += `<span class="algo-metric">${k}: <b>${typeof v === 'number' ? v.toFixed(3) : v}</b></span>`;
      }
    }

    container.insertAdjacentHTML('beforeend', `
      <div class="algo-box">
        <div class="algo-name">${a.name}</div>
        <div class="algo-decision">
          <span class="badge ${decisionCls}">${a.lastDecision || '—'}</span>
          <div class="signal-bar-wrap">
            <div class="signal-bar" style="left:${sigLeft}%;width:${sigWidth}%;background:${sigColor}"></div>
            <div class="signal-center"></div>
          </div>
          <span style="color:var(--muted);font-size:10px">signal: ${signal.toFixed(2)}</span>
        </div>
        <div class="algo-reasoning">${a.reasoning || ''}</div>
        <div class="algo-metrics">${metricsHtml}</div>
      </div>`);
  });
}

// ── Charts ────────────────────────────────────────────────
function initCharts() {
  const chartDefaults = {
    animation: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { display: false },
      y: { grid: { color: 'rgba(37,42,56,0.6)' }, ticks: { color: '#5a6278', font: { size: 9 } } }
    }
  };

  priceChart = new Chart(document.getElementById('price-chart'), {
    type: 'line',
    data: { labels: [], datasets: [] },
    options: { ...chartDefaults, maintainAspectRatio: false }
  });

  pnlChart = new Chart(document.getElementById('pnl-chart'), {
    type: 'line',
    data: {
      labels: Array(MAX_PNL).fill(''),
      datasets: [{
        data: [], borderColor: '#00d68f', borderWidth: 1.5,
        fill: true, backgroundColor: 'rgba(0,214,143,0.05)',
        tension: 0.3, pointRadius: 0
      }]
    },
    options: { ...chartDefaults, maintainAspectRatio: false }
  });
}

const CHART_COLORS = ['#4e8ef7','#00d68f','#ffc107','#ff4d6a','#7c5cbf'];
function updatePriceChart(quotes) {
  const symbols = quotes.map(q => q.symbol).sort();
  priceChart.data.datasets = symbols.map((sym, i) => ({
    label: sym,
    data: (priceData[sym] || []).map((p, idx) => ({
      x: idx, y: p
    })),
    borderColor: CHART_COLORS[i % CHART_COLORS.length],
    borderWidth: 1.2, pointRadius: 0, tension: 0.2,
    fill: false,
    yAxisID: 'y'
  }));
  priceChart.data.labels = Array(30).fill('');
  priceChart.update('none');
}

function updatePnlChart() {
  pnlChart.data.datasets[0].data = pnlHistory;
  pnlChart.data.datasets[0].borderColor = (pnlHistory[pnlHistory.length - 1] || 0) >= 0
    ? 'var(--green)' : 'var(--red)';
  pnlChart.update('none');
}

// ── Helpers ───────────────────────────────────────────────
function fmt(n)     { return '$' + (n || 0).toFixed(2); }
function fmtK(n)    { const v = n || 0; return (v >= 0 ? '$' : '-$') + Math.abs(v / 1000).toFixed(1) + 'K'; }
function fmtPnL(n)  { return (n >= 0 ? '+$' : '-$') + Math.abs(n || 0).toFixed(0); }

function styleValue(id, val) {
  document.getElementById(id).className = 'value ' + (val >= 0 ? 'pos' : 'neg');
}
function setBar(id, pct, color) {
  const el = document.getElementById(id);
  el.style.width      = pct + '%';
  el.style.background = color;
}
function shortStrategy(s) {
  if (!s) return '';
  if (s.includes('Pairs')) return 'PAIRS';
  if (s.includes('Delta')) return 'DELTA';
  return s.substring(0, 8);
}
function decisionClass(d) {
  if (!d) return 'badge-red';
  if (d.includes('TRADE') || d.includes('HEDGE') || d.includes('CLOSE')) return 'badge-green';
  if (d.includes('HOLD')) return 'badge-yellow';
  if (d.includes('BLOCKED') || d.includes('WARMUP')) return 'badge-red';
  return 'badge-yellow';
}

// ── Clock ─────────────────────────────────────────────────
function updateClock() {
  document.getElementById('clock').textContent = new Date().toLocaleTimeString();
}

// ── Init ──────────────────────────────────────────────────
window.addEventListener('load', () => {
  initCharts();
  connect();
  updateClock();
  setInterval(updateClock, 1000);
});
