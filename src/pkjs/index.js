var ACTION_NEW_NOTE = 1;        // watch -> phone: single freshly recorded note
var ACTION_SYNC_NOTES = 2;      // phone -> watch: authoritative note list (streamed)
var ACTION_UPDATE_SETTINGS = 3; // phone -> watch: settings
var ACTION_REQUEST_SYNC = 4;    // phone -> watch: "send me all your notes"
var ACTION_WATCH_NOTES = 5;     // watch -> phone: full watch note list (streamed)

var STORAGE_KEY_NOTES = 'pebble_voice_notes_list';
var STORAGE_KEY_SETTINGS = 'pebble_voice_notes_settings';
var STORAGE_KEY_DELETED = 'pebble_voice_notes_deleted';

var DEFAULT_TTL = 604800;   // 1 week in seconds
var WATCH_MAX_NOTES = 20;   // Must match MAX_NOTES in src/c/model.h
var NOTE_TEXT_MAX = 159;    // Must match NOTE_TEXT_MAX_LEN - 1 in src/c/model.h
var SEND_MAX_RETRIES = 3;
var TOMBSTONE_MAX = 500;
var TOMBSTONE_FOREVER_SECS = 365 * 24 * 3600;
var WATCH_BATCH_TIMEOUT_MS = 10000;

// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------

function nowSecs() {
  return Math.floor(Date.now() / 1000);
}

function noteTime(n) {
  return n.time || n.timestamp || 0;
}

function sortNotes(notes) {
  return notes.sort(function(a, b) {
    return noteTime(b) - noteTime(a);
  });
}

function readJson(key, fallback) {
  try {
    var raw = localStorage.getItem(key);
    if (raw) {
      return JSON.parse(raw);
    }
  } catch (e) {
    console.error('Failed to read ' + key + ': ' + e);
  }
  return fallback;
}

function writeJson(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch (e) {
    console.error('Failed to write ' + key + ': ' + e);
  }
}

function getStoredNotes() {
  var notes = readJson(STORAGE_KEY_NOTES, []);
  return Array.isArray(notes) ? notes : [];
}

function saveStoredNotes(notes) {
  writeJson(STORAGE_KEY_NOTES, sortNotes(notes));
}

function getStoredSettings() {
  var s = readJson(STORAGE_KEY_SETTINGS, null);
  if (!s) {
    s = { ttl: DEFAULT_TTL, quickLaunch: 1 };
  }
  return s;
}

function saveStoredSettings(settings) {
  writeJson(STORAGE_KEY_SETTINGS, settings);
}

// Tombstones: { "<noteId>": <noteTimeSecs> } for notes deleted on the phone,
// so a stale copy on the watch can't resurrect them on the next sync.
function getTombstones() {
  var t = readJson(STORAGE_KEY_DELETED, {});
  return (t && typeof t === 'object') ? t : {};
}

function saveTombstones(tombstones, ttl) {
  var now = nowSecs();
  var maxAge = (ttl && ttl > 0) ? ttl + 86400 : TOMBSTONE_FOREVER_SECS;
  var entries = [];
  for (var id in tombstones) {
    if (tombstones.hasOwnProperty(id)) {
      // Once a note is past its TTL, the watch has expired it too: drop the tombstone
      if (now - tombstones[id] <= maxAge) {
        entries.push([id, tombstones[id]]);
      }
    }
  }
  entries.sort(function(a, b) { return b[1] - a[1]; });
  var pruned = {};
  for (var i = 0; i < entries.length && i < TOMBSTONE_MAX; i++) {
    pruned[entries[i][0]] = entries[i][1];
  }
  writeJson(STORAGE_KEY_DELETED, pruned);
}

function cleanupExpiredNotes(notes, ttl) {
  if (!ttl || ttl <= 0) {
    return notes;
  }
  var now = nowSecs();
  return notes.filter(function(n) {
    return (now - noteTime(n)) <= ttl;
  });
}

// Merge notes coming from the watch into phone storage.
// The phone is authoritative for notes it already knows (it holds edits),
// and tombstoned (deleted) notes are never re-added.
function mergeWatchNotes(incoming) {
  var settings = getStoredSettings();
  var notes = getStoredNotes();
  var tombstones = getTombstones();
  var known = {};
  var added = 0;

  for (var i = 0; i < notes.length; i++) {
    known[notes[i].id] = true;
  }
  for (var j = 0; j < incoming.length; j++) {
    var n = incoming[j];
    if (!n || !n.text || known[n.id] || tombstones.hasOwnProperty(String(n.id))) {
      continue;
    }
    notes.push({ id: n.id, time: noteTime(n), text: n.text });
    known[n.id] = true;
    added++;
  }

  var cleaned = cleanupExpiredNotes(notes, settings.ttl);
  saveStoredNotes(cleaned);
  return { notes: sortNotes(cleaned), added: added };
}

function watchSubset(notes) {
  return sortNotes(notes.slice()).slice(0, WATCH_MAX_NOTES);
}

function sameNoteLists(a, b) {
  if (a.length !== b.length) return false;
  for (var i = 0; i < a.length; i++) {
    if (a[i].id !== b[i].id ||
        noteTime(a[i]) !== noteTime(b[i]) ||
        (a[i].text || '').substring(0, NOTE_TEXT_MAX) !== (b[i].text || '').substring(0, NOTE_TEXT_MAX)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Sequential AppMessage queue (one in flight at a time, with retries)
// ---------------------------------------------------------------------------

var s_queue = [];
var s_sending = false;

function enqueue(dict) {
  s_queue.push({ dict: dict, retries: 0 });
  pumpQueue();
}

function pumpQueue() {
  if (s_sending || s_queue.length === 0) return;
  s_sending = true;
  var item = s_queue[0];
  Pebble.sendAppMessage(item.dict, function() {
    s_queue.shift();
    s_sending = false;
    pumpQueue();
  }, function() {
    s_sending = false;
    if (item.retries >= SEND_MAX_RETRIES) {
      console.error('Giving up on message action ' + item.dict.AppKeyAction);
      // A partial note batch is useless; drop the rest of it
      var dropAction = item.dict.AppKeyAction;
      s_queue.shift();
      if (dropAction === ACTION_SYNC_NOTES) {
        s_queue = s_queue.filter(function(q) { return q.dict.AppKeyAction !== ACTION_SYNC_NOTES; });
      }
      pumpQueue();
      return;
    }
    item.retries++;
    setTimeout(pumpQueue, 500);
  });
}

function sendSettingsToWatch(settings) {
  enqueue({
    AppKeyAction: ACTION_UPDATE_SETTINGS,
    SettingTTL: settings.ttl,
    SettingQuickLaunchRecord: settings.quickLaunch ? 1 : 0
  });
}

function sendNotesToWatch(notes) {
  // Supersede any not-yet-sent batch (never interrupt the in-flight message)
  s_queue = s_queue.filter(function(q, idx) {
    return (s_sending && idx === 0) || q.dict.AppKeyAction !== ACTION_SYNC_NOTES;
  });

  var subset = watchSubset(notes);
  if (subset.length === 0) {
    enqueue({ AppKeyAction: ACTION_SYNC_NOTES, NoteCount: 0, NoteIndex: 0 });
    return;
  }
  for (var i = 0; i < subset.length; i++) {
    enqueue({
      AppKeyAction: ACTION_SYNC_NOTES,
      NoteCount: subset.length,
      NoteIndex: i,
      NoteId: subset[i].id,
      NoteTime: noteTime(subset[i]),
      NoteText: (subset[i].text || '').substring(0, NOTE_TEXT_MAX)
    });
  }
  console.log('Queued ' + subset.length + ' notes for watch');
}

// ---------------------------------------------------------------------------
// Watch -> phone batch collection & reconciliation
// ---------------------------------------------------------------------------

var s_watchBatch = null;
var s_watchBatchTimer = null;

function finishWatchBatch() {
  if (s_watchBatchTimer) {
    clearTimeout(s_watchBatchTimer);
    s_watchBatchTimer = null;
  }
  var watchNotes = s_watchBatch || [];
  s_watchBatch = null;

  var result = mergeWatchNotes(watchNotes);
  var desired = watchSubset(result.notes);
  var watchHas = sortNotes(watchNotes.slice());

  console.log('Watch sync: watch=' + watchNotes.length + ', phone=' + result.notes.length +
              ', newFromWatch=' + result.added);

  sendSettingsToWatch(getStoredSettings());
  if (!sameNoteLists(desired, watchHas)) {
    console.log('Watch list is out of date; pushing ' + desired.length + ' notes');
    sendNotesToWatch(result.notes);
  }
}

function handleWatchNotesPacket(dict) {
  var count = dict.NoteCount || 0;
  var index = dict.NoteIndex || 0;

  if (index === 0) {
    s_watchBatch = [];
    if (s_watchBatchTimer) clearTimeout(s_watchBatchTimer);
    s_watchBatchTimer = setTimeout(function() {
      console.error('Watch note batch timed out; discarding');
      s_watchBatch = null;
      s_watchBatchTimer = null;
    }, WATCH_BATCH_TIMEOUT_MS);
  }
  if (!s_watchBatch) return;

  if (count > 0 && dict.NoteText !== undefined) {
    s_watchBatch.push({ id: dict.NoteId, time: dict.NoteTime, text: dict.NoteText });
  }
  if (count === 0 || index >= count - 1) {
    finishWatchBatch();
  }
}

// ---------------------------------------------------------------------------
// PebbleKit JS events
// ---------------------------------------------------------------------------

Pebble.addEventListener('ready', function() {
  console.log('PebbleKit JS ready for Voice Notes');
  var settings = getStoredSettings();
  var notes = getStoredNotes();
  var cleaned = cleanupExpiredNotes(notes, settings.ttl);
  if (cleaned.length !== notes.length) {
    saveStoredNotes(cleaned);
  }
  // Ask the watch for its notes; reconciliation happens when the batch completes
  enqueue({ AppKeyAction: ACTION_REQUEST_SYNC });
});

Pebble.addEventListener('appmessage', function(e) {
  var dict = e.payload;
  if (!dict) return;

  var action = dict.AppKeyAction;

  if (action === ACTION_NEW_NOTE) {
    var result = mergeWatchNotes([{ id: dict.NoteId, time: dict.NoteTime, text: dict.NoteText }]);
    console.log('New note from watch saved. Total notes on phone: ' + result.notes.length);
  } else if (action === ACTION_WATCH_NOTES) {
    handleWatchNotesPacket(dict);
  }
});

Pebble.addEventListener('showConfiguration', function() {
  var settings = getStoredSettings();
  var notes = cleanupExpiredNotes(getStoredNotes(), settings.ttl);

  var payload = {
    ttl: settings.ttl,
    quickLaunch: settings.quickLaunch,
    notes: notes
  };

  console.log('Opening configuration page with ' + notes.length + ' notes');
  var html = getConfigHtml(payload);
  var dataUri = 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
  Pebble.openURL(dataUri);
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response || e.response === 'CANCELLED' || e.response === 'undefined') {
    console.log('Configuration cancelled');
    return;
  }

  var result = null;
  try {
    try {
      result = JSON.parse(decodeURIComponent(e.response));
    } catch (e1) {
      result = JSON.parse(e.response);
    }
  } catch (err) {
    console.error('Failed to parse config result: ' + err);
    return;
  }
  if (!result) return;

  var ttl = result.ttl !== undefined ? parseInt(result.ttl, 10) : DEFAULT_TTL;
  var quickLaunch = result.quickLaunch !== undefined ? (result.quickLaunch ? 1 : 0) : 1;
  var newNotes = Array.isArray(result.notes) ? result.notes : [];

  var settings = { ttl: ttl, quickLaunch: quickLaunch };
  saveStoredSettings(settings);

  // Record deletions so stale copies on the watch can't bring them back
  var oldNotes = getStoredNotes();
  var keptIds = {};
  for (var i = 0; i < newNotes.length; i++) {
    keptIds[newNotes[i].id] = true;
  }
  var tombstones = getTombstones();
  for (var j = 0; j < oldNotes.length; j++) {
    if (!keptIds[oldNotes[j].id]) {
      tombstones[String(oldNotes[j].id)] = noteTime(oldNotes[j]);
    }
  }
  saveTombstones(tombstones, ttl);

  var cleanedNotes = cleanupExpiredNotes(newNotes, ttl);
  saveStoredNotes(cleanedNotes);
  console.log('Settings saved: ttl=' + ttl + ', notes=' + cleanedNotes.length);

  // Push settings, then the authoritative note list, to the watch
  sendSettingsToWatch(settings);
  sendNotesToWatch(cleanedNotes);
});

function getConfigHtml(payload) {
  var initialJson = JSON.stringify(payload || { ttl: DEFAULT_TTL, quickLaunch: 1, notes: [] })
    .replace(/</g, '\\u003c')
    .replace(/>/g, '\\u003e')
    .replace(/\u2028/g, '\\u2028')
    .replace(/\u2029/g, '\\u2029');
  return [
    '<!DOCTYPE html><html><head><meta charset="UTF-8">',
    '<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">',
    '<title>Voice Notes Settings</title><style>',
    ':root{--primary:#0066cc;--primary-dark:#004d99;--bg:#f4f6f8;--card-bg:#ffffff;--text:#1d2129;--text-muted:#65676b;--danger:#e53935;--border:#e4e6eb;--radius:10px}',
    '*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}',
    'body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background-color:var(--bg);color:var(--text);line-height:1.4;padding:16px 16px 96px 16px;overflow-x:hidden}',
    'header{margin-bottom:20px;text-align:center}',
    'header h1{font-size:22px;font-weight:700;color:var(--primary)}',
    'header p{font-size:13px;color:var(--text-muted);margin-top:4px}',
    '.section-title{font-size:14px;font-weight:600;text-transform:uppercase;letter-spacing:.5px;color:var(--text-muted);margin:20px 0 8px 4px;display:flex;justify-content:space-between;align-items:center}',
    '.swipe-hint{font-size:11px;font-weight:500;text-transform:none;letter-spacing:0;color:var(--text-muted)}',
    '.card{background:var(--card-bg);border-radius:var(--radius);border:1px solid var(--border);padding:14px 16px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,.05)}',
    '.form-group{margin-bottom:14px}.form-group:last-child{margin-bottom:0}',
    'label{display:block;font-size:14px;font-weight:600;margin-bottom:6px}',
    '.help-text{font-size:12px;color:var(--text-muted);margin-top:4px}',
    'select,textarea{width:100%;padding:10px 12px;font-size:15px;border:1px solid var(--border);border-radius:8px;background:#fafbfc;color:var(--text);outline:0;font-family:inherit}',
    'select:focus,textarea:focus{border-color:var(--primary);background:#fff}',
    '.switch-label{display:flex;align-items:center;justify-content:space-between;cursor:pointer}',
    '.switch{position:relative;display:inline-block;width:44px;height:24px}.switch input{opacity:0;width:0;height:0}',
    '.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#ccc;transition:.3s;border-radius:24px}',
    '.slider:before{position:absolute;content:"";height:18px;width:18px;left:3px;bottom:3px;background-color:#fff;transition:.3s;border-radius:50%}',
    'input:checked+.slider{background-color:var(--primary)}input:checked+.slider:before{transform:translateX(20px)}',
    '.note-swipe-wrap{position:relative;margin-bottom:12px;border-radius:var(--radius);overflow:hidden;background:var(--danger)}',
    '.swipe-bg{position:absolute;inset:0;display:flex;align-items:center;justify-content:space-between;padding:0 20px;color:#fff;font-weight:700;font-size:14px;pointer-events:none}',
    '.note-card{position:relative;background:var(--card-bg);border-radius:var(--radius);border:1px solid var(--border);padding:12px 14px;transition:transform .2s ease,opacity .2s ease;will-change:transform;touch-action:pan-y}',
    '.note-card.dragging{transition:none}',
    '.note-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}',
    '.note-time{font-size:13px;font-weight:600;color:var(--primary)}',
    '.btn-delete{background:none;border:none;color:var(--danger);font-size:13px;font-weight:600;cursor:pointer;padding:4px 8px;border-radius:4px}',
    '.btn-delete:active{background:#fee}',
    '.note-textarea{width:100%;resize:vertical;min-height:56px;font-size:14px;line-height:1.4}',
    '.empty-state{text-align:center;padding:28px 16px;color:var(--text-muted);font-size:14px}',
    '.btn-secondary{background:#eef3f8;color:var(--primary);border:1px dashed var(--primary);border-radius:8px;padding:10px;width:100%;font-size:14px;font-weight:600;cursor:pointer;margin-bottom:16px}',
    '.toast{position:fixed;left:16px;right:16px;bottom:76px;background:#20232a;color:#fff;padding:10px 14px;border-radius:8px;display:none;align-items:center;justify-content:space-between;font-size:14px;box-shadow:0 4px 12px rgba(0,0,0,.2);z-index:10}',
    '.toast.show{display:flex}',
    '.toast button{background:none;border:none;color:#61dafb;font-weight:700;font-size:14px;cursor:pointer;padding:4px 8px}',
    '.action-bar{position:fixed;bottom:0;left:0;right:0;background:#fff;border-top:1px solid var(--border);padding:12px 16px;display:flex;gap:12px;box-shadow:0 -2px 10px rgba(0,0,0,.06);z-index:20}',
    '.btn{flex:1;padding:12px;font-size:15px;font-weight:600;border-radius:8px;border:none;cursor:pointer;text-align:center}',
    '.btn-cancel{background:#f0f2f5;color:var(--text)}.btn-primary{background:var(--primary);color:#fff}',
    '</style></head><body>',
    '<header><h1>Voice Notes Settings</h1><p>Manage voice memos and auto-cleanup preferences</p></header>',
    '<div class="section-title"><span>Preferences</span></div>',
    '<div class="card">',
    '<div class="form-group"><label for="ttl-select">Default Note Expiration (TTL)</label>',
    '<select id="ttl-select"><option value="86400">1 Day</option><option value="259200">3 Days</option><option value="604800">1 Week (Default)</option><option value="1209600">2 Weeks</option><option value="2592000">1 Month (30 Days)</option><option value="0">Keep Forever (No Expiration)</option></select>',
    '<div class="help-text">Notes older than this threshold will be automatically purged.</div></div>',
    '<div class="form-group" style="margin-top:16px;padding-top:14px;border-top:1px solid var(--border)">',
    '<label class="switch-label"><span>Quick Launch Auto-Record</span><label class="switch"><input type="checkbox" id="quick-launch-toggle"><span class="slider"></span></label></label>',
    '<div class="help-text">Directly open the microphone when opened via Quick Launch shortcut.</div></div></div>',
    '<div class="section-title"><span>Your Voice Notes (<span id="notes-count">0</span>)</span><span class="swipe-hint">Swipe card to delete</span></div>',
    '<button type="button" class="btn-secondary" id="btn-add-note">+ Add Text Note Manually</button>',
    '<div id="notes-container"></div>',
    '<div class="toast" id="undo-toast"><span>Note deleted</span><button type="button" id="btn-undo">Undo</button></div>',
    '<div class="action-bar"><button type="button" class="btn btn-cancel" id="btn-cancel">Cancel</button><button type="button" class="btn btn-primary" id="btn-save">Save & Sync to Watch</button></div>',
    '<script>var INITIAL_DATA = ' + initialJson + ';',
    '(function(){',
    'var s_notes=INITIAL_DATA.notes||[],s_ttl=INITIAL_DATA.ttl!==undefined?INITIAL_DATA.ttl:604800,s_quickLaunch=INITIAL_DATA.quickLaunch!==undefined?INITIAL_DATA.quickLaunch:1;',
    'var s_lastDeleted=null,s_undoTimer=null;',
    'function formatTime(t){var d=new Date(t*1000);return d.toLocaleDateString(undefined,{weekday:"short",month:"short",day:"numeric",year:"numeric"})+" \\u2022 "+d.toLocaleTimeString(undefined,{hour:"numeric",minute:"2-digit"})}',
    'function showUndo(note,idx){s_lastDeleted={note:note,idx:idx};var toast=document.getElementById("undo-toast");toast.classList.add("show");if(s_undoTimer)clearTimeout(s_undoTimer);s_undoTimer=setTimeout(function(){toast.classList.remove("show");s_lastDeleted=null;},4000);}',
    'document.getElementById("btn-undo").onclick=function(){if(!s_lastDeleted)return;var pos=Math.min(s_lastDeleted.idx,s_notes.length);s_notes.splice(pos,0,s_lastDeleted.note);s_lastDeleted=null;if(s_undoTimer)clearTimeout(s_undoTimer);document.getElementById("undo-toast").classList.remove("show");render();};',
    'function deleteNoteAt(idx,cardEl,dir){var removed=s_notes[idx];if(cardEl){cardEl.classList.remove("dragging");cardEl.style.transform="translateX("+(dir<0?"-110%":"110%")+")";cardEl.style.opacity="0";setTimeout(function(){s_notes.splice(idx,1);render();showUndo(removed,idx);},180);}else{s_notes.splice(idx,1);render();showUndo(removed,idx);}}',
    'function attachSwipe(card,idx){var startX=0,startY=0,dx=0,tracking=false,locked=null;',
    'card.addEventListener("touchstart",function(e){if(!e.touches||e.touches.length!==1)return;startX=e.touches[0].clientX;startY=e.touches[0].clientY;dx=0;tracking=true;locked=null;},{passive:true});',
    'card.addEventListener("touchmove",function(e){if(!tracking||!e.touches||e.touches.length!==1)return;var x=e.touches[0].clientX-startX;var y=e.touches[0].clientY-startY;if(!locked){if(Math.abs(x)<8&&Math.abs(y)<8)return;locked=Math.abs(x)>Math.abs(y)?"h":"v";if(locked==="h")card.classList.add("dragging");}if(locked==="h"){if(e.cancelable)e.preventDefault();dx=x;card.style.transform="translateX("+dx+"px)";}},{passive:false});',
    'function endSwipe(){if(!tracking)return;tracking=false;if(locked!=="h")return;card.classList.remove("dragging");var threshold=Math.min(90,card.offsetWidth*0.32);if(Math.abs(dx)>=threshold){deleteNoteAt(idx,card,dx<0?-1:1);}else{card.style.transform="translateX(0)";}}',
    'card.addEventListener("touchend",endSwipe,{passive:true});',
    'card.addEventListener("touchcancel",endSwipe,{passive:true});}',
    'function render(){document.getElementById("ttl-select").value=s_ttl;document.getElementById("quick-launch-toggle").checked=(s_quickLaunch===1);document.getElementById("notes-count").textContent=s_notes.length;var c=document.getElementById("notes-container");c.innerHTML="";if(s_notes.length===0){var empty=document.createElement("div");empty.className="empty-state card";empty.textContent="No voice notes recorded yet. Record on your watch or add one above!";c.appendChild(empty);return}',
    's_notes.forEach(function(note,idx){var wrap=document.createElement("div");wrap.className="note-swipe-wrap";var bg=document.createElement("div");bg.className="swipe-bg";bg.innerHTML="<span>Delete</span><span>Delete</span>";wrap.appendChild(bg);var card=document.createElement("div");card.className="note-card";var h=document.createElement("div");h.className="note-header";var t=document.createElement("div");t.className="note-time";t.textContent=formatTime(note.time||note.timestamp);var del=document.createElement("button");del.className="btn-delete";del.textContent="Delete";del.onclick=function(){deleteNoteAt(idx,card,-1);};h.appendChild(t);h.appendChild(del);var ta=document.createElement("textarea");ta.className="note-textarea";ta.value=note.text||"";ta.maxLength=159;ta.oninput=function(){note.text=ta.value};card.appendChild(h);card.appendChild(ta);attachSwipe(card,idx);wrap.appendChild(card);c.appendChild(wrap);});}',
    'document.getElementById("btn-add-note").onclick=function(){var txt=prompt("Enter note text:");if(txt&&txt.trim().length>0){var now=Math.floor(Date.now()/1000);s_notes.unshift({id:now,time:now,text:txt.trim().substring(0,159)});render()}};',
    'document.getElementById("btn-cancel").onclick=function(){window.location.href="pebblejs://close#"};',
    'document.getElementById("btn-save").onclick=function(){var ttlVal=parseInt(document.getElementById("ttl-select").value,10);var qlVal=document.getElementById("quick-launch-toggle").checked?1:0;var clean=s_notes.filter(function(n){return n.text&&n.text.trim().length>0});var res={ttl:ttlVal,quickLaunch:qlVal,notes:clean};window.location.href="pebblejs://close#"+encodeURIComponent(JSON.stringify(res))};',
    'render();})();</script></body></html>'
  ].join('');
}
