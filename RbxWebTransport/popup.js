// Popup: launch the official web client for a place, or open the protocol
// inspector pages. The web client itself is served by Roblox and playtest
// gated per universe; this popup only builds the same URL the Play button
// uses, so the normal Roblox session cookie in the browser is what signs in.

const $ = (id) => document.getElementById(id);
const place = $('place');
const statusEl = $('status');

function setStatus(text, cls = '') {
  statusEl.textContent = text;
  statusEl.className = 'status ' + cls;
}

chrome.storage.local.get({ lastPlace: '1818' }).then(({ lastPlace }) => {
  if (!place.value) place.value = lastPlace;
  place.focus();
  place.select();
});

function openPlace() {
  const id = place.value.trim();
  if (!/^\d+$/.test(id)) {
    setStatus('Enter a numeric place ID', 'err');
    return;
  }
  chrome.storage.local.set({ lastPlace: id });
  const url = `https://play.roblox.com/games/start?placeId=${encodeURIComponent(id)}`;
  chrome.tabs.create({ url });
  setStatus(`opened ${url}`, 'ok');
}

function openPage(path) {
  chrome.tabs.create({ url: chrome.runtime.getURL(path) });
  setStatus(`opened ${path}`, 'ok');
}

$('launch').addEventListener('click', openPlace);
place.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') openPlace();
});
$('inspector').addEventListener('click', () => openPage('client.html'));
$('headless').addEventListener('click', () => openPage('client-headless.html'));
