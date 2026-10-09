// Background service worker: opens the headless client (no UI) in a tab.
// The page auto-runs auth -> join -> WebTransport connect for DEFAULT_PLACE_ID
// (place 1818) and logs everything to its console / window.__rbx.
chrome.action.onClicked.addListener(() => {
  chrome.tabs.create({ url: chrome.runtime.getURL('src/client-headless.html') });
});
