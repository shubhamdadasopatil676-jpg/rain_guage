const CONFIG = {
  refreshIntervalMs: 10000,
  funnelArea_cm2: 78.5,
  maxRainRateForBar: 30,
  maxTipsForBar: 60
};

let chartCanvas, chartCtx;
let currentData = null;
let lastSeenTotalTips = null;

async function fetchSensorData() {
  const res = await fetch('/data');
  if (!res.ok) {
    throw new Error(`/data returned HTTP ${res.status}`);
  }
  return await res.json();
}

function deriveMetrics(data) {
  const totalVolume_mL = data.totalTipsToday * data.mlPerTip;
  const rainfall_mm = (totalVolume_mL / CONFIG.funnelArea_cm2) * 10;

  const intervals = data.recentTipIntervalsSec;
  const avgIntervalSec = intervals.length
    ? intervals.reduce((a, b) => a + b, 0) / intervals.length
    : 0;

  const mlPerHourAtCurrentPace = avgIntervalSec > 0
    ? (data.mlPerTip / avgIntervalSec) * 3600
    : 0;
  const rainRate_mmPerHr = (mlPerHourAtCurrentPace / CONFIG.funnelArea_cm2) * 10;

  const lastTipMs = new Date(data.lastTipTimestamp).getTime();
  const secondsSinceLastTip = Math.max(0, Math.floor((Date.now() - lastTipMs) / 1000));

  return {
    totalVolume_mL,
    rainfall_mm,
    avgIntervalSec,
    rainRate_mmPerHr,
    secondsSinceLastTip
  };
}

function formatTimeAgo(seconds) {
  if (seconds < 60) return `${seconds}s ago`;
  const mins = Math.floor(seconds / 60);
  if (mins < 60) return `${mins} min ago`;
  const hrs = Math.floor(mins / 60);
  return `${hrs}h ${mins % 60}m ago`;
}

function formatClockTime(iso) {
  const d = new Date(iso);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function pct(value, max) {
  return Math.max(0, Math.min(100, (value / max) * 100));
}

function render(data) {
  currentData = data;
  const m = deriveMetrics(data);

  document.getElementById('stationName').textContent = data.stationName;
  document.getElementById('stationLocation').textContent = data.stationLocation;
  document.getElementById('lastUpdated').textContent = formatClockTime(data.lastUpdated);
  document.getElementById('rainfallValue').textContent = m.rainfall_mm.toFixed(1);
  document.getElementById('miniTipCount').textContent = data.totalTipsToday;

  const statusText = document.getElementById('statusText');
  const statusPill = document.getElementById('statusPill');
  if (m.secondsSinceLastTip < 600) {
    statusText.textContent = 'Raining';
    statusPill.style.opacity = 1;
  } else if (data.totalTipsToday > 0) {
    statusText.textContent = 'Dry — rain earlier today';
    statusPill.style.opacity = 0.75;
  } else {
    statusText.textContent = 'Dry';
    statusPill.style.opacity = 0.6;
  }

  document.getElementById('statTotalTips').textContent = data.totalTipsToday;
  document.getElementById('barTotalTips').style.width = pct(data.totalTipsToday, CONFIG.maxTipsForBar) + '%';

  document.getElementById('statVolume').textContent = Math.round(m.totalVolume_mL);
  document.getElementById('barVolume').style.width = pct(m.totalVolume_mL, CONFIG.maxTipsForBar * data.mlPerTip) + '%';

  document.getElementById('statRainfall').textContent = m.rainfall_mm.toFixed(1);
  document.getElementById('barRainfall').style.width = pct(m.rainfall_mm, 25) + '%';

  document.getElementById('statRate').textContent = m.rainRate_mmPerHr.toFixed(1);
  document.getElementById('barRate').style.width = pct(m.rainRate_mmPerHr, CONFIG.maxRainRateForBar) + '%';

  document.getElementById('statLastTip').textContent = formatTimeAgo(m.secondsSinceLastTip);
  document.getElementById('barLastTip').style.width = pct(600 - Math.min(600, m.secondsSinceLastTip), 600) + '%';

  document.getElementById('statInterval').textContent = Math.round(m.avgIntervalSec);
  document.getElementById('barInterval').style.width = pct(120 - Math.min(120, m.avgIntervalSec), 120) + '%';

  document.getElementById('calibNote').textContent = `${data.mlPerTip.toFixed(1)} mL / tip`;
  document.getElementById('funnelNote').textContent = `${CONFIG.funnelArea_cm2} cm²`;

  drawChart(data.hourlyTips);

  if (lastSeenTotalTips !== null && data.totalTipsToday > lastSeenTotalTips) {
    animateBucketTip();
  }
  lastSeenTotalTips = data.totalTipsToday;
}

function setupCanvas() {
  chartCanvas = document.getElementById('tipsChart');
  chartCtx = chartCanvas.getContext('2d');
  resizeCanvas();
  window.addEventListener('resize', () => {
    resizeCanvas();
    if (currentData) drawChart(currentData.hourlyTips);
  });
}

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const rect = chartCanvas.getBoundingClientRect();
  chartCanvas.width = rect.width * dpr;
  chartCanvas.height = 220 * dpr;
  chartCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
}

function drawChart(hourlyTips) {
  const width = chartCanvas.getBoundingClientRect().width;
  const height = 220;
  const padding = { top: 16, right: 12, bottom: 28, left: 28 };
  const plotW = width - padding.left - padding.right;
  const plotH = height - padding.top - padding.bottom;

  const maxTip = Math.max(4, ...hourlyTips.map(h => h.tips));
  const stepX = plotW / (hourlyTips.length - 1);

  const points = hourlyTips.map((h, i) => ({
    x: padding.left + i * stepX,
    y: padding.top + plotH - (h.tips / maxTip) * plotH,
    tips: h.tips,
    hour: h.hour
  }));

  chartCtx.clearRect(0, 0, width, height);

  chartCtx.strokeStyle = '#e7edf5';
  chartCtx.lineWidth = 1;
  for (let g = 0; g <= 3; g++) {
    const y = padding.top + (plotH / 3) * g;
    chartCtx.beginPath();
    chartCtx.moveTo(padding.left, y);
    chartCtx.lineTo(width - padding.right, y);
    chartCtx.stroke();
  }

  chartCtx.beginPath();
  chartCtx.moveTo(points[0].x, padding.top + plotH);
  points.forEach(p => chartCtx.lineTo(p.x, p.y));
  chartCtx.lineTo(points[points.length - 1].x, padding.top + plotH);
  chartCtx.closePath();
  const gradient = chartCtx.createLinearGradient(0, padding.top, 0, padding.top + plotH);
  gradient.addColorStop(0, 'rgba(79, 216, 224, 0.35)');
  gradient.addColorStop(1, 'rgba(79, 216, 224, 0.02)');
  chartCtx.fillStyle = gradient;
  chartCtx.fill();

  chartCtx.beginPath();
  points.forEach((p, i) => i === 0 ? chartCtx.moveTo(p.x, p.y) : chartCtx.lineTo(p.x, p.y));
  chartCtx.strokeStyle = '#2fb8c2';
  chartCtx.lineWidth = 2.5;
  chartCtx.lineJoin = 'round';
  chartCtx.stroke();

  chartCtx.font = "11px 'JetBrains Mono', monospace";
  chartCtx.fillStyle = '#5c6c85';
  chartCtx.textAlign = 'center';
  points.forEach((p, i) => {
    chartCtx.beginPath();
    chartCtx.arc(p.x, p.y, 3, 0, Math.PI * 2);
    chartCtx.fillStyle = '#0b1f3a';
    chartCtx.fill();

    if (i % 2 === 0) {
      chartCtx.fillStyle = '#5c6c85';
      chartCtx.fillText(p.hour, p.x, height - 8);
    }
  });
}

function animateBucketTip() {
  const bucket = document.getElementById('bucketGroup');
  const water = document.getElementById('bucketWater');
  const drop = document.getElementById('feedDrop');

  water.setAttribute('d', 'M86 150 L134 150 L134 138 L86 138 Z');
  drop.style.opacity = '1';

  setTimeout(() => {
    bucket.style.transform = 'rotate(35deg)';
    water.style.opacity = '0.15';
  }, 500);

  setTimeout(() => {
    bucket.style.transform = 'rotate(0deg)';
    water.setAttribute('d', 'M86 150 L134 150 L134 150 L86 150 Z');
    water.style.opacity = '0.55';
    drop.style.opacity = '0';
  }, 1300);
}

async function refresh() {
  try {
    const data = await fetchSensorData();
    render(data);
  } catch (err) {
    console.error('Failed to fetch sensor data:', err);
    document.getElementById('statusText').textContent = 'Sensor unreachable';
  }
}

window.addEventListener('DOMContentLoaded', () => {
  setupCanvas();
  refresh();
  setInterval(refresh, CONFIG.refreshIntervalMs);
});
