// Phone side of the ZenNotes viewer. Mirror of src/c/main.c.
//
// Fetches notes from the ZenNotes server API and streams them to the watch:
//   GET {server}/api/notes                 -> note list (NoteMeta[])
//   GET {server}/api/notes/read?path=...   -> { body, ... }
// Both take "Authorization: Bearer <token>".
//
// The note list is fetched once per root load and cached; folder browsing,
// "recent" and search all work from that cache.

var highlighter = require('./highlight');

var CMD_LIST = 1;
var CMD_OPEN = 2;
var CMD_SEARCH = 3;
var CMD_REFRESH = 4;
var CMD_PAGE = 5;
var CMD_LINK = 6;
var CMD_BACK = 7;
var CMD_IMAGE = 8;

var KIND_LIST = 1;
var KIND_NOTE = 2;
var KIND_ERROR = 3;
var KIND_DISPLAY = 4;
var KIND_IMAGE = 5;

var MAX_ENTRIES = 100;    // keep in sync with MAX_ENTRIES in main.c
var MAX_DEPTH = 12;       // keep in sync with MAX_DEPTH in main.c
var MAX_PAYLOAD = 32000;  // under MAX_PAYLOAD (32768) in main.c
// Longer notes are split into pages of about this size, at block boundaries.
// At 30 KB, a page of Japanese text in the large font could lay out taller
// than the watch's 16-bit coordinates allow.
var PAGE_BYTES = 16000;
var MAX_PAGES = 10;       // page numbers travel as a single digit
var MAX_TITLE = 60;
// Watch inbox is opened at app_message_inbox_size_maximum(); stay well below it.
var CHUNK_SIZE = 2000;

var THEMES = { dark: 0, light: 1 };
var LANGS = { ja: 0, en: 1 };
// How far one UP/DOWN click moves on the watch (ScrollMode in main.c).
var SCROLL_STEPS = { small: 0, half: 1, page: 2 };

// Text the phone sends to the watch or shows in the settings page.
var STRINGS = {
  ja: {
    noServer: 'スマホのアプリ設定でサーバーのURLを入れてください',
    noToken: 'スマホのアプリ設定でトークンを入れてください',
    badToken: 'トークンが正しくありません。スマホのアプリ設定で確認してください',
    serverError: 'サーバーでエラーが起きました (%s)',
    badResponse: 'サーバーの応答を読み取れませんでした',
    offline: '接続できません。スマホのTailscaleがオンか確認してください',
    timeout: 'サーバーから応答がありません。少し待ってから再読み込みしてください',
    emptyData: '空のデータです',
    noNotes: 'ノートがありません',
    stale: '見つかりません。一覧を開き直してください',
    notFound: '「%s」は見つかりませんでした',
    search: '検索',
    recent: '最近',
    tags: 'タグ',
    searchHeader: '検索: %s',
    truncated: '…(以下省略)',
    image: '[画像]',
    imageNotFound: '画像が見つかりません',
    imageFailed: '画像を読み込めません',
    imageTooLarge: '画像が大きすぎて開けません',
    untitled: '(無題)',
    nextPage: '続きを読む %s',
    prevPage: '前のページ %s',
    offlineTag: 'オフライン',
    savedOn: 'オフライン表示 (%s 保存)',
    notSaved: 'オフラインです。このノートは保存されていません'
  },
  en: {
    noServer: 'Enter your server URL in the app settings on your phone',
    noToken: 'Enter your token in the app settings on your phone',
    badToken: 'The token was rejected. Check it in the app settings on your phone',
    serverError: 'The server returned an error (%s)',
    badResponse: 'Could not read the server response',
    offline: 'Cannot connect. Check that Tailscale is on on your phone',
    timeout: 'The server did not respond. Wait a moment and reload',
    emptyData: 'Nothing to show',
    noNotes: 'No notes',
    stale: 'Not found. Open the list again',
    notFound: 'Nothing matches "%s"',
    search: 'Search',
    recent: 'Recent',
    tags: 'Tags',
    searchHeader: 'Search: %s',
    truncated: '… (truncated)',
    image: '[image]',
    imageNotFound: 'Image not found',
    imageFailed: 'Could not load the image',
    imageTooLarge: 'The image is too large to open',
    untitled: '(untitled)',
    nextPage: 'Continue reading %s',
    prevPage: 'Previous page %s',
    offlineTag: 'offline',
    savedOn: 'Offline copy (saved %s)',
    notSaved: 'You are offline and this note was not saved'
  }
};

function t(key, arg) {
  var text = STRINGS[loadSettings().lang][key];
  return arg === undefined ? text : text.replace('%s', arg);
}

function phoneLang() {
  var lang = '';
  try {
    lang = navigator.language || '';
  } catch (e) { /* not available in this runtime */ }
  return /^ja/i.test(lang) ? 'ja' : 'en';
}

var DEFAULTS = {
  server: '',
  token: '',
  folder: '',
  theme: 'dark',
  font: 'large',
  status: 'on',
  scrollbar: 'off',
  scroll: 'small'
};

// Cached notes (trash and anything outside the folder filter removed).
var s_notes = [];
// Entries of the list shown at each depth on the watch, by row index, and
// how each list was made (so it can be rebuilt after a refresh).
var s_levels = [];
var s_levelSpecs = [];
// Pages of the note currently open on the watch.
var s_pages = [];
var s_path = '';      // note on the watch
var s_links = [];     // its wikilinks that point at a known note: { label, path }
var s_history = [];   // notes left by following a link, for BACK
var s_images = [];    // its embedded images: { label, target, wiki }
var s_transfer = 0;

function log(msg) {
  console.log('[zennotes] ' + msg);
}

function loadSettings() {
  var cfg = {};
  try {
    cfg = JSON.parse(localStorage.getItem('settings') || '{}') || {};
  } catch (e) { /* use defaults */ }
  return {
    server: (cfg.server || DEFAULTS.server).replace(/\/+$/, ''),
    token: cfg.token || DEFAULTS.token,
    folder: (cfg.folder || DEFAULTS.folder).replace(/^\/+|\/+$/g, ''),
    theme: THEMES.hasOwnProperty(cfg.theme) ? cfg.theme : DEFAULTS.theme,
    font: cfg.font === 'normal' ? 'normal' : DEFAULTS.font,
    status: cfg.status === 'off' ? 'off' : 'on',
    scrollbar: cfg.scrollbar === 'on' ? 'on' : 'off',
    scroll: SCROLL_STEPS.hasOwnProperty(cfg.scroll) ? cfg.scroll : 'small',
    lang: LANGS.hasOwnProperty(cfg.lang) ? cfg.lang : phoneLang()
  };
}

function saveSettings(cfg) {
  try {
    localStorage.setItem('settings', JSON.stringify(cfg));
  } catch (e) { log('could not save settings'); }
}

// ---- HTTP ----

function apiGet(path, callback) {
  var cfg = loadSettings();
  if (!cfg.server) {
    callback(t('noServer'));
    return;
  }
  if (!cfg.token) {
    callback(t('noToken'));
    return;
  }
  // Answer exactly once: some PebbleKit JS runtimes never fire ontimeout, so
  // a timer of our own gives up on a server that doesn't answer.
  var done = false;
  function finish(err, data, offline) {
    if (done) {
      return;
    }
    done = true;
    clearTimeout(timer);
    callback(err, data, offline);
  }
  var xhr = new XMLHttpRequest();
  var timer = setTimeout(function () {
    try { xhr.abort(); } catch (e) { /* ignore */ }
    finish(t('timeout'), null, true);
  }, 15000);
  xhr.open('GET', cfg.server + path, true);
  xhr.setRequestHeader('Authorization', 'Bearer ' + cfg.token);
  xhr.timeout = 15000;
  xhr.onload = function () {
    if (xhr.status === 401) {
      finish(t('badToken'));
      return;
    }
    if (xhr.status < 200 || xhr.status >= 300) {
      // 5xx: the server is there but can't answer; treat like being offline.
      finish(t('serverError', xhr.status), null, xhr.status >= 500);
      return;
    }
    var data;
    try {
      data = JSON.parse(xhr.responseText);
    } catch (e) {
      finish(t('badResponse'));
      return;
    }
    finish(null, data);
  };
  xhr.onerror = function () {
    // Network failure (or a CORS rejection; both look the same from here).
    finish(t('offline'), null, true);
  };
  xhr.ontimeout = function () {
    finish(t('timeout'), null, true);
  };
  xhr.send();
}

// ---- Sending to the watch ----

function utf8Bytes(str) {
  var bin = unescape(encodeURIComponent(str));
  var out = new Array(bin.length);
  for (var i = 0; i < bin.length; i++) {
    out[i] = bin.charCodeAt(i);
  }
  return out;
}

function byteLength(str) {
  return unescape(encodeURIComponent(str)).length;
}

// Trim a string so its UTF-8 form fits in maxBytes, without splitting a
// character, and append suffix.
function fitBytes(str, maxBytes, suffix) {
  if (byteLength(str) <= maxBytes) {
    return str;
  }
  var budget = maxBytes - byteLength(suffix);
  var lo = 0;
  var hi = str.length;
  while (lo < hi) {
    var mid = Math.ceil((lo + hi) / 2);
    if (byteLength(str.slice(0, mid)) <= budget) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return str.slice(0, lo) + suffix;
}

// AppMessage sends are often refused when the link is busy: retry a few
// times with a growing delay. alive() lets a superseded transfer stop.
function sendMessage(msg, ok, alive, fail, attempt) {
  attempt = attempt || 0;
  Pebble.sendAppMessage(msg, ok, function () {
    if (alive && !alive()) {
      return;
    }
    if (attempt >= 4) {
      log('send failed after ' + (attempt + 1) + ' attempts');
      if (fail) {
        fail();
      }
      return;
    }
    setTimeout(function () {
      if (!alive || alive()) {
        sendMessage(msg, ok, alive, fail, attempt + 1);
      }
    }, 250 * (attempt + 1));
  });
}

function sendPayload(kind, text, extra) {
  sendBytes(kind, utf8Bytes(text), extra);
}

function sendBytes(kind, bytes, extra) {
  var total = bytes.length;
  var token = ++s_transfer;
  var offset = 0;
  var restarts = 0;

  if (total === 0) {
    sendError(t('emptyData'));
    return;
  }

  function next() {
    if (token !== s_transfer || offset >= total) {
      return;
    }
    var end = Math.min(offset + CHUNK_SIZE, total);
    var msg = { KIND: kind, OFFSET: offset, TOTAL: total, CHUNK: bytes.slice(offset, end) };
    for (var k in extra) {
      msg[k] = extra[k];
    }
    sendMessage(msg, function () {
      offset = end;
      next();
    }, function () {
      return token === s_transfer;
    }, function () {
      // A chunk never got through: start the whole payload again, since the
      // watch only accepts chunks in order.
      if (restarts < 2) {
        restarts++;
        offset = 0;
        setTimeout(next, 1000);
      }
    });
  }
  next();
}

function sendError(text) {
  log('error: ' + escape(text));  // ASCII only: pebble logs breaks on split UTF-8
  s_transfer++;
  sendMessage({ KIND: KIND_ERROR, TEXT: text });
}

function sendDisplay(done) {
  var cfg = loadSettings();
  var sent = false;
  function once() {
    if (!sent) {
      sent = true;
      done();
    }
  }
  sendMessage(
    {
      KIND: KIND_DISPLAY,
      THEME: THEMES[cfg.theme],
      FONT: cfg.font === 'large' ? 1 : 0,
      STATUS: cfg.status === 'on' ? 1 : 0,
      SCROLLBAR: cfg.scrollbar === 'on' ? 1 : 0,
      LANG: LANGS[cfg.lang],
      SCROLL: SCROLL_STEPS[cfg.scroll]
    },
    once);
  // Carry on with the list even if the settings never get through.
  setTimeout(once, 3000);
}

// ---- Markdown to watch blocks ----
// Each output line is a tag character, an indent digit and the text; see the
// note payload description in main.c.

// Characters the watch fonts can't draw (they show as boxes), checked on a
// Pebble Time 2 with the Japanese language pack, mapped to ones it can.
var GLYPH_MAP = {
  '─': '-', '━': '-', '═': '=',                  // ─ ━ ═
  '│': '|', '┃': '|', '║': '|',                  // │ ┃ ║
  '├': '|', '└': '`',                                 // ├ └  (tree: "|--", "`--")
  '☆': '*', '◎': '○', '▼': '↓',        // ☆→* ◎→○ ▼→↓
  '✓': '✔', '⌘': 'Cmd',                          // ✓→✔ ⌘
  '⸻': '——', '⸺': '——', '―': '—'  // ⸻ ⸺ ―
};
var ROMAN = ['I', 'II', 'III', 'IV', 'V', 'VI', 'VII', 'VIII', 'IX', 'X', 'XI', 'XII'];

// Make text drawable on the watch: plain spaces for special spaces, no
// zero-width or combining marks, and stand-ins for missing symbols.
function cleanText(text) {
  return String(text)
    .replace(/[​-‍⁠﻿︎️⃣￼]/g, '')
    .replace(/　/g, '  ')
    .replace(/[  -   ]/g, ' ')
    .replace(/[─-╿═-╬☆◎▼✓⌘⸺⸻―]/g,
      function (ch) {
        // Box-drawing corners and junctions other than the tree ones: '+'.
        return GLYPH_MAP.hasOwnProperty(ch) ? GLYPH_MAP[ch] : '+';
      })
    .replace(/[①-⑳]/g, function (ch) {                 // ①..⑳
      return '(' + (ch.charCodeAt(0) - 0x2460 + 1) + ')';
    })
    .replace(/[Ⅰ-Ⅻ]/g, function (ch) {                 // Ⅰ..Ⅻ
      return ROMAN[ch.charCodeAt(0) - 0x2160];
    })
    .replace(/[ⅰ-ⅻ]/g, function (ch) {                 // ⅰ..ⅻ
      return ROMAN[ch.charCodeAt(0) - 0x2170].toLowerCase();
    });
}

// ---- Wikilinks ----
// [[target]], [[target|label]], [[target#heading]]: resolved like Obsidian,
// by path first, then by note name (same folder, then shortest path).

var MAX_LINKS = 40;
var LINK_LABEL_BYTES = 60;
var LINK_MARK = '\x02';  // ends a block's text: index of its first link, '+' if more

var s_linkCtx = null;  // while a note is converted: { from, list, pending }

function stripMd(path) {
  return path.replace(/\.md$/i, '');
}

function normalizePath(path) {
  var out = [];
  path.split('/').forEach(function (part) {
    if (part === '..') {
      out.pop();
    } else if (part && part !== '.') {
      out.push(part);
    }
  });
  return out.join('/');
}

function resolveLink(target, from) {
  var name = stripMd(target.split('#')[0].split('^')[0].trim());
  if (!name) {
    return null;
  }
  var folder = from.split('/').slice(0, -1).join('/');
  if (/^\.\.?\//.test(name)) {
    name = normalizePath((folder ? folder + '/' : '') + name);
  }
  name = name.replace(/^\/+/, '');
  var lower = name.toLowerCase();
  var exact = null;
  var best = null;
  s_notes.forEach(function (n) {
    var bare = stripMd(n.path || '');
    var low = bare.toLowerCase();
    if (low === lower) {
      exact = n.path;
    } else if (low.slice(-lower.length - 1) === '/' + lower) {
      var near = bare.split('/').slice(0, -1).join('/') === folder;
      if (!best || (near && !best.near) ||
          (near === best.near && n.path.length < best.path.length)) {
        best = { path: n.path, near: near };
      }
    }
  });
  return exact || (best && best.path);
}

function linkText(target, label) {
  var ctx = s_linkCtx;
  var path = ctx && resolveLink(target, ctx.from);
  if (!path) {
    return label;
  }
  var index = -1;
  for (var i = 0; i < ctx.list.length; i++) {
    if (ctx.list[i].path === path) {
      index = i;
    }
  }
  if (index < 0 && ctx.list.length < MAX_LINKS) {
    index = ctx.list.length;
    ctx.list.push({ label: label, path: path });
  }
  if (index >= 0) {
    ctx.pending.push(index);
  }
  return '\ud83d\udd17' + label;  // 🔗: shows on the watch (checked on the device)
}

function inline(text) {
  return text
    .replace(/\t/g, ' ')
    .replace(/!\[\[([^\]]*)\]\]/g, '$1')  // an embedded note: its name
    .replace(/!\[[^\]]*\]\([^)]*\)/g, t('image'))
    .replace(/\[\[([^\]|]*)\|([^\]]*)\]\]/g, function (all, target, label) {
      return linkText(target, label.trim());
    })
    .replace(/\[\[([^\]]*)\]\]/g, function (all, target) {
      var label = target.split('#')[0].trim().split('/').pop() || target;
      if (target.indexOf('#') > 0) {
        label += ' > ' + target.split('#').slice(1).join('#').replace(/^\^/, '');
      }
      return linkText(target, label);
    })
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/(\*\*|__)(.+?)\1/g, '$2')
    .replace(/(^|[^*\w])\*([^*\s][^*]*)\*/g, '$1$2')
    .replace(/~~(.+?)~~/g, '$1')
    .replace(/==(.+?)==/g, '$1')
    .replace(/`([^`]*)`/g, '$1')
    .replace(/<br\s*\/?>/gi, ' ')
    .replace(/<[^>]+>/g, '');
}

function indentOf(spaces) {
  var n = Math.floor(spaces.replace(/\t/g, '    ').length / 2);
  return String(Math.min(n, 9));
}

// Obsidian callout types -> colour slot on the watch (see Theme.callout).
var CALLOUT_COLORS = {
  note: 0, info: 0, todo: 0,
  tip: 1, hint: 1, important: 1, abstract: 1, summary: 1, tldr: 1,
  success: 2, check: 2, done: 2,
  question: 3, help: 3, faq: 3, warning: 3, caution: 3, attention: 3,
  failure: 4, fail: 4, missing: 4, danger: 4, error: 4, bug: 4,
  example: 5,
  quote: 6, cite: 6
};

var TABLE_SEPARATOR = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/;

function tableCells(line) {
  return line.trim().replace(/\\\|/g, '\u0001').replace(/^\||\|$/g, '').split('|')
    .map(function (c) { return inline(c.replace(/\u0001/g, '|').trim()); });
}

// Tables stay tables: one line per row with cells separated by tabs (G for
// the header row, g for the rest). The watch lays out the grid and lets a
// table wider than the screen scroll sideways.
function tableBlocks(rows) {
  return rows.map(function (r, i) {
    return (i === 0 ? 'G0' : 'g0') + r.join('\t');
  });
}

// Line inside a callout: keep simple list markers readable without the
// nesting the watch payload can't express.
function calloutLine(text) {
  return inline(text
    .replace(/^\s*[-*+]\s+\[ \]\s*/, '□ ')
    .replace(/^\s*[-*+]\s+\[[xX]\]\s*/, '■ ')
    .replace(/^\s*[-*+]\s+/, '・')
    .replace(/^#{1,6}\s+/, ''));
}

// ---- Images ----
// ![[file.png]] and ![alt](url) become I blocks of their own ("I0<index>\t
// <label>"); the watch opens one in its image viewer, and the phone fetches,
// fits and dithers it then (openImage).

var MAX_IMAGES = 20;
var IMAGE_EXT = /\.(png|jpe?g|gif|webp|svg|avif|bmp)$/i;
var IMAGE_RE = /!\[\[([^\]]+)\]\]|!\[([^\]]*)\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)/g;

// Takes the images out of a line; returns the rest of the line.
function takeImages(line, found) {
  return line.replace(IMAGE_RE, function (all, wiki, alt, url) {
    // Markdown escapes (\&, \_) are not part of the URL.
    var target = wiki ? wiki.split('|')[0].trim() : url.replace(/\\([^\w\s])/g, '$1');
    if (wiki && !IMAGE_EXT.test(target)) {
      return all;  // an embedded note, not an image
    }
    if (s_images.length >= MAX_IMAGES) {
      return '';
    }
    var label = (alt || '').trim() ||
      decodeURIComponentSafe(target.split(/[?#]/)[0].split('/').pop()) || t('image');
    found.push(s_images.length);
    s_images.push({ label: label, target: target, wiki: !!wiki });
    return '';
  });
}

function decodeURIComponentSafe(s) {
  try {
    return decodeURIComponent(s);
  } catch (e) {
    return s;
  }
}

function mdToBlocks(title, meta, md, from) {
  s_linkCtx = { from: from || '', list: [], pending: [] };
  s_images = [];
  var text = cleanText(md).replace(/\r\n?/g, '\n').replace(/^---\n[\s\S]*?\n---\n/, '');
  var lines = text.split('\n');
  var out = ['t0' + inline(cleanText(title)), 'm0' + cleanText(meta)];
  s_linkCtx.pending = [];
  var code = null;  // { lang, lines } while inside a fenced code block
  var callout = null;  // colour slot of the open callout

  function push(line) {
    // Collapse runs of blank lines, and drop blanks right after the title.
    if (line === 'b0' && /^(b|t|m)/.test(out[out.length - 1])) {
      return;
    }
    var pending = s_linkCtx.pending;
    if (pending.length) {
      line += LINK_MARK + pending[0] + (pending.length > 1 ? '+' : '');
      s_linkCtx.pending = [];
    }
    out.push(line);
  }

  var images = [];  // image blocks of the previous line, pushed after it
  function flushImages() {
    images.forEach(function (index) {
      push('I0' + index + '\t' + fitBytes(cleanText(s_images[index].label), 60, '…'));
    });
    images = [];
  }

  for (var i = 0; i < lines.length; i++) {
    flushImages();
    var line = lines[i];
    var m;
    // Code block: highlighted lines (k), then a footer (K) with the language
    // name and line count. The watch previews a few lines and opens the rest
    // in its code viewer.
    if ((m = line.match(/^\s*(```|~~~)\s*([\w+#.-]*)/))) {
      if (!code) {
        code = { lang: m[2], lines: [] };
        continue;
      }
      highlighter.highlight(code.lang, code.lines).forEach(function (l) {
        out.push('k0' + (l || ' '));
      });
      out.push('K0' + highlighter.languageName(code.lang) + '\t' + code.lines.length);
      code = null;
      continue;
    }
    if (code) {
      code.lines.push(line.replace(/\t/g, '  ').replace(/[\x00-\x08]/g, ''));
      continue;
    }
    line = takeImages(line, images);
    if (images.length && /^\s*([-*+]|\d+[.)]|>)?\s*$/.test(line)) {
      continue;  // the line held only images (maybe as a list item)
    }
    if (callout !== null) {
      if (/^\s*>/.test(line)) {
        push('Q' + callout + calloutLine(line.replace(/^\s*(>\s?)+/, '')));
        continue;
      }
      callout = null;
    }
    if (/^\s*$/.test(line)) {
      push('b0');
    } else if ((m = line.match(/^\s*>\s?\[!(\w+)\][+-]?\s*(.*)$/))) {
      var type = m[1].toLowerCase();
      callout = CALLOUT_COLORS.hasOwnProperty(type) ? CALLOUT_COLORS[type] : 0;
      push('C' + callout + inline(m[2] || type.charAt(0).toUpperCase() + type.slice(1)));
    } else if (/^\s*\|/.test(line) && i + 1 < lines.length && TABLE_SEPARATOR.test(lines[i + 1])) {
      var rows = [tableCells(line)];
      for (i += 2; i < lines.length && /^\s*\|/.test(lines[i]); i++) {
        rows.push(tableCells(lines[i]));
      }
      i--;
      s_linkCtx.pending = [];  // a table row can't be tapped open; the outline lists them
      tableBlocks(rows).forEach(push);
    } else if ((m = line.match(/^(#{1,6})\s+(.*?)\s*#*\s*$/))) {
      push(Math.min(m[1].length, 3) + '0' + inline(m[2]));
    } else if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) {
      push('r0');
    } else if ((m = line.match(/^(\s*)[-*+]\s+\[([ xX])\]\s*(.*)$/))) {
      push((m[2] === ' ' ? 'c' : 'x') + indentOf(m[1]) + inline(m[3]));
    } else if ((m = line.match(/^(\s*)[-*+]\s+(.*)$/))) {
      push('l' + indentOf(m[1]) + inline(m[2]));
    } else if ((m = line.match(/^(\s*)(\d+[.)])\s+(.*)$/))) {
      push('o' + indentOf(m[1]) + m[2] + ' ' + inline(m[3]));
    } else if (/^\s*>/.test(line)) {
      push('q0' + inline(line.replace(/^\s*(>\s?)+/, '')));
    } else {
      push('p0' + inline(line.trim()));
    }
  }
  flushImages();
  if (code) {
    // Unclosed fence: show what there is.
    highlighter.highlight(code.lang, code.lines).forEach(function (l) {
      out.push('k0' + (l || ' '));
    });
    out.push('K0' + highlighter.languageName(code.lang) + '\t' + code.lines.length);
  }
  while (out.length > 1 && out[out.length - 1] === 'b0') {
    out.pop();
  }
  s_links = s_linkCtx.list;
  s_linkCtx = null;
  return out.join('\n');
}

// One J line per link, in index order, for the watch's outline.
function linkLines() {
  return s_links.map(function (l) {
    return 'J0' + fitBytes(l.label, LINK_LABEL_BYTES, '…');
  });
}

// ---- Lists ----

function noteTitle(n) {
  var base = (n.path || '').split('/').pop().replace(/\.md$/i, '');
  var title = cleanText(n.title || base || t('untitled')).replace(/\s*\n\s*/g, ' ');
  return title.length > MAX_TITLE ? title.slice(0, MAX_TITLE) + '…' : title;
}

function byUpdated(a, b) {
  return (b.updatedAt || 0) - (a.updatedAt || 0);
}

// updatedAt may arrive as milliseconds, seconds or an ISO string.
function toDate(v) {
  if (typeof v === 'number') {
    return new Date(v < 1e12 ? v * 1000 : v);
  }
  var ms = Date.parse(v);
  return isNaN(ms) ? null : new Date(ms);
}

function pad2(n) {
  return (n < 10 ? '0' : '') + n;
}

// "10/8" this year, "2025/10/8" otherwise.
function shortDate(v) {
  var d = toDate(v);
  if (!d) {
    return '';
  }
  var md = (d.getMonth() + 1) + '/' + d.getDate();
  return d.getFullYear() === new Date().getFullYear() ? md : d.getFullYear() + '/' + md;
}

function fullDate(v) {
  var d = toDate(v);
  if (!d) {
    return '';
  }
  return d.getFullYear() + '/' + pad2(d.getMonth() + 1) + '/' + pad2(d.getDate());
}

function noteEntry(n) {
  return { type: 'N', path: n.path, title: noteTitle(n) };
}

// For lists that mix folders ("recent", search results), show when each note
// was last updated.
function datedEntry(n) {
  var e = noteEntry(n);
  e.date = shortDate(n.updatedAt);
  return e;
}

// Entries directly inside folder (a path without trailing slash, '' = root):
// subfolders first by name, then notes newest first.
function folderEntries(folder) {
  var prefix = folder ? folder + '/' : '';
  var folders = {};
  var notes = [];
  s_notes.forEach(function (n) {
    var path = n.path || '';
    if (path.indexOf(prefix) !== 0) {
      return;
    }
    var rest = path.slice(prefix.length);
    var slash = rest.indexOf('/');
    if (slash >= 0) {
      folders[rest.slice(0, slash)] = true;
    } else {
      notes.push(n);
    }
  });
  // Plain code-point order: locale-aware compare isn't available in every
  // PebbleKit JS runtime (the emulator throws an ICU error).
  var names = Object.keys(folders).sort(function (a, b) {
    var x = a.toLowerCase();
    var y = b.toLowerCase();
    return x < y ? -1 : (x > y ? 1 : 0);
  });
  notes.sort(byUpdated);
  return names.map(function (name) {
    return { type: 'D', path: prefix + name, title: name };
  }).concat(notes.map(noteEntry));
}

function recentEntries() {
  return s_notes.slice().sort(byUpdated).map(datedEntry);
}

function normalize(s) {
  s = String(s || '');
  try {
    s = s.normalize('NFKC');
  } catch (e) { /* not available in this runtime */ }
  return s.toLowerCase();
}

// Tags of a note as plain names ("#camp" and "camp" are the same tag).
function noteTags(n) {
  var tags = n.tags;
  if (typeof tags === 'string') {
    tags = tags.split(/[,\s]+/);
  }
  if (!tags || !tags.length) {
    return [];
  }
  return tags.map(function (tg) {
    return cleanText(String(tg)).replace(/^#/, '').trim();
  }).filter(function (tg) { return tg; });
}

// All tags with how many notes carry each, by name.
function tagEntries() {
  var counts = {};
  s_notes.forEach(function (n) {
    noteTags(n).forEach(function (tg) {
      counts[tg] = (counts[tg] || 0) + 1;
    });
  });
  return Object.keys(counts).sort(function (a, b) {
    var x = a.toLowerCase();
    var y = b.toLowerCase();
    return x < y ? -1 : (x > y ? 1 : 0);
  }).map(function (tg) {
    return { type: 'H', tag: tg, title: tg, date: String(counts[tg]) };
  });
}

function taggedEntries(tag) {
  return s_notes.filter(function (n) {
    return noteTags(n).indexOf(tag) >= 0;
  }).sort(byUpdated).map(datedEntry);
}

// Every word of the query must appear in the title, path or tags.
function searchEntries(query) {
  var terms = normalize(query).replace(/[。、．，.,!?！？]/g, ' ')
    .split(/[\s　]+/).filter(function (t) { return t; });
  if (terms.length === 0) {
    return [];
  }
  return s_notes.filter(function (n) {
    var hay = normalize(noteTitle(n) + ' ' + n.path + ' ' + noteTags(n).join(' '))
      .replace(/[\s　]+/g, '');
    return terms.every(function (t) { return hay.indexOf(t) >= 0; });
  }).sort(byUpdated).map(datedEntry);
}

// Store entries as the list at this depth and send them to the watch.
function sendList(level, header, entries, spec) {
  if (level >= MAX_DEPTH) {
    return;
  }
  var text = header;
  var kept = [];
  for (var i = 0; i < entries.length && kept.length < MAX_ENTRIES; i++) {
    var line = '\n' + entries[i].type + entries[i].title +
      (entries[i].date ? '\t' + entries[i].date : '');
    if (byteLength(text + line) > MAX_PAYLOAD) {
      break;
    }
    text += line;
    kept.push(entries[i]);
  }
  s_levels.length = level;
  s_levels[level] = kept;
  s_levelSpecs.length = level;
  s_levelSpecs[level] = spec;
  log('sending ' + kept.length + ' entries at level ' + level);
  sendPayload(KIND_LIST, text, { LEVEL: level });
}

// ---- Commands ----

// ---- Offline copies ----
// The note list and the most recently opened notes are kept in localStorage,
// so they can still be read when the server can't be reached.

var CACHE_NOTES = 30;           // notes kept
var CACHE_CHARS = 1500000;      // rough cap on stored text
var s_offline = false;          // the list on the watch came from the cache

function cacheGet(key) {
  try {
    return JSON.parse(localStorage.getItem(key) || 'null');
  } catch (e) {
    return null;
  }
}

function cacheSet(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
    return true;
  } catch (e) {
    return false;  // storage full
  }
}

function cacheList(notes) {
  cacheSet('cache:list', notes.map(function (n) {
    return { path: n.path, title: n.title, folder: n.folder, updatedAt: n.updatedAt,
      tags: n.tags };
  }));
}

// Keep a note, most recent first, dropping the oldest past the limits.
function cacheNote(path, note) {
  var index = (cacheGet('cache:index') || []).filter(function (p) { return p !== path; });
  index.unshift(path);
  var entry = { title: note.title, body: note.body || '', updatedAt: note.updatedAt,
    savedAt: Date.now() };
  var total = 0;
  var kept = [];
  index.forEach(function (p) {
    var size = p === path ? entry.body.length : ((cacheGet('cache:n:' + p) || {}).body || '').length;
    if (kept.length < CACHE_NOTES && total + size <= CACHE_CHARS) {
      kept.push(p);
      total += size;
    } else {
      try { localStorage.removeItem('cache:n:' + p); } catch (e) { /* ignore */ }
    }
  });
  // If storage is full, drop the oldest kept notes until it fits.
  while (!cacheSet('cache:n:' + path, entry) && kept.length > 1) {
    var drop = kept.pop();
    try { localStorage.removeItem('cache:n:' + drop); } catch (e) { /* ignore */ }
  }
  cacheSet('cache:index', kept);
}

function filterNotes(notes) {
  var folder = loadSettings().folder;
  var prefix = folder ? folder + '/' : '';
  return (notes || []).filter(function (n) {
    return n.folder !== 'trash' && (n.path || '').indexOf(prefix) === 0;
  });
}

// Fetch the note list into memory, falling back to the saved copy offline.
function loadNotes(callback) {
  apiGet('/api/notes', function (err, notes, offline) {
    if (err) {
      var saved = offline ? cacheGet('cache:list') : null;
      if (!saved) {
        callback(err);
        return;
      }
      s_offline = true;
      s_notes = filterNotes(saved);
    } else {
      s_offline = false;
      cacheList(notes || []);
      s_notes = filterNotes(notes);
    }
    callback(s_notes.length === 0 ? t('noNotes') : null);
  });
}

// Search and Recent always; Tags only when some note has tags.
function rootActions() {
  var actions = [{ type: 'S', title: t('search') }, { type: 'R', title: t('recent') }];
  var tagged = s_notes.some(function (n) { return noteTags(n).length > 0; });
  if (tagged) {
    actions.push({ type: 'T', title: t('tags') });
  }
  return actions;
}

// Build and send the list described by spec at this depth.
function sendSpec(level, spec) {
  if (spec.kind === 'root') {
    var folder = loadSettings().folder;
    sendList(level, (folder || 'ZenNotes') + (s_offline ? ' (' + t('offlineTag') + ')' : ''),
      rootActions().concat(folderEntries(folder)), spec);
  } else if (spec.kind === 'folder') {
    sendList(level, spec.path, folderEntries(spec.path), spec);
  } else if (spec.kind === 'recent') {
    sendList(level, t('recent'), recentEntries(), spec);
  } else if (spec.kind === 'tags') {
    sendList(level, t('tags'), tagEntries(), spec);
  } else if (spec.kind === 'tag') {
    sendList(level, '#' + spec.tag, taggedEntries(spec.tag), spec);
  } else if (spec.kind === 'search') {
    var results = searchEntries(spec.query);
    if (results.length === 0) {
      sendError(t('notFound', spec.query));
      return;
    }
    sendList(level, t('searchHeader', spec.query), results, spec);
  }
}

function fetchList() {
  loadNotes(function (err) {
    if (err) {
      sendError(err);
      return;
    }
    sendSpec(0, { kind: 'root' });
  });
}

// Re-fetch the notes and resend the list at this depth.
function refreshLevel(level) {
  var spec = s_levelSpecs[level] || { kind: 'root' };
  loadNotes(function (err) {
    if (err) {
      sendError(err);
      return;
    }
    sendSpec(level, spec);
  });
}

function openEntry(level, index) {
  var entry = (s_levels[level] || [])[index];
  if (!entry) {
    sendError(t('stale'));
    return;
  }
  if (entry.type === 'D') {
    sendSpec(level + 1, { kind: 'folder', path: entry.path });
  } else if (entry.type === 'R') {
    sendSpec(level + 1, { kind: 'recent' });
  } else if (entry.type === 'T') {
    sendSpec(level + 1, { kind: 'tags' });
  } else if (entry.type === 'H') {
    sendSpec(level + 1, { kind: 'tag', tag: entry.tag });
  } else if (entry.type === 'N') {
    s_history = [];
    fetchNote(entry.path);
  }
}

function search(level, query) {
  sendSpec(level + 1, { kind: 'search', query: query });
}

// A page may not start inside a code block, table or callout.
function canBreakBefore(line) {
  return 'kKgGQ'.indexOf(line.charAt(0)) < 0;
}

// Split note blocks into pages that fit the watch. Every page repeats the
// title and meta lines, and links to its neighbours with P/N blocks (the
// digit is the target page).
function paginate(blocks) {
  var lines = blocks.split('\n');
  var head = lines.slice(0, 2);
  var body = lines.slice(2);
  var links = linkLines();
  var headBytes = byteLength(head.concat(links).join('\n')) + 1;
  var budget = PAGE_BYTES - 200;  // room for the page links
  var pages = [];
  var cur = [];
  var size = headBytes;
  for (var i = 0; i < body.length && pages.length < MAX_PAGES - 1; i++) {
    var line = fitBytes(body[i], 4000, '…');
    var bytes = byteLength(line) + 1;
    if (cur.length && ((size + bytes > budget && canBreakBefore(line)) ||
                       size + bytes > PAGE_BYTES - 120)) {
      pages.push(cur);
      cur = [];
      size = headBytes;
    }
    cur.push(line);
    size += bytes;
  }
  if (i < body.length) {
    // Past the last page: keep what fits and say the rest was cut.
    cur = cur.concat(body.slice(i));
  }
  pages.push(cur);
  var n = pages.length;
  return pages.map(function (page, p) {
    var out = head.slice();
    if (p > 0) {
      out.push('P' + (p - 1) + t('prevPage', p + '/' + n));
    }
    out = out.concat(page);
    if (p < n - 1) {
      out.push('N' + (p + 1) + t('nextPage', (p + 2) + '/' + n));
    }
    out = out.concat(links);
    return fitBytes(out.join('\n'), MAX_PAYLOAD, '\np0' + t('truncated'));
  });
}

function sendPage(index) {
  if (!s_pages[index]) {
    sendError(t('stale'));
    return;
  }
  sendPayload(KIND_NOTE, s_pages[index]);
}

function showNote(path, note, metaExtra) {
  var title = note.title || path.split('/').pop().replace(/\.md$/i, '');
  // Folder and last update (or where the copy came from), under the title.
  var cached = s_notes.filter(function (n) { return n.path === path; })[0] || {};
  var folder = path.split('/').slice(0, -1).join('/');
  var date = metaExtra || fullDate(note.updatedAt || cached.updatedAt);
  var meta = [folder, date].filter(function (x) { return x; }).join('  ') || ' ';
  s_path = path;
  s_pages = paginate(mdToBlocks(title, meta, note.body || '', path));
  sendPage(0);
}

function fetchNote(path) {
  apiGet('/api/notes/read?path=' + encodeURIComponent(path), function (err, note, offline) {
    if (err) {
      var saved = offline ? cacheGet('cache:n:' + path) : null;
      if (saved) {
        showNote(path, saved, t('savedOn', fullDate(saved.savedAt)));
      } else {
        sendError(offline ? t('notSaved') : err);
      }
      return;
    }
    cacheNote(path, note);
    showNote(path, note);
  });
}

function openLink(index) {
  var link = s_links[index];
  if (!link) {
    sendError(t('stale'));
    return;
  }
  s_history.push(s_path);
  fetchNote(link.path);
}

// BACK after following links: the note we came from.
function linkBack() {
  var path = s_history.pop();
  if (!path) {
    sendError(t('stale'));
    return;
  }
  fetchNote(path);
}

// ---- Image viewer ----
// The phone's script runtime decodes images itself (createImageBitmap, or an
// <img> for SVG); the picture is fitted to the screen, dithered to the
// watch's 64 colours and sent as one byte per pixel (0b11rrggbb).

var IMAGE_MAX_BYTES = 24 * 1024 * 1024;  // animated WebP demos run to ~16 MB
var s_assets = null;  // /api/assets, fetched the first time a ![[name]] is opened

// Largest picture the viewer shows: the screen (round: the square inside it).
function imageBox() {
  var info = Pebble.getActiveWatchInfo ? Pebble.getActiveWatchInfo() : null;
  var platform = info && info.platform;
  if (platform === 'gabbro') {
    return { w: 180, h: 180 };
  }
  if (platform === 'chalk') {
    return { w: 126, h: 126 };
  }
  if (platform === 'emery') {
    return { w: 200, h: 228 };
  }
  return { w: 144, h: 168 };
}

function assetUrl(path) {
  return { url: loadSettings().server + '/api/assets/raw?path=' + encodeURIComponent(path), auth: true };
}

// Where an image lives: a URL as is; a path relative to the note; an
// Obsidian ![[name]] looked up by name among the vault's files.
function imageSource(img, callback) {
  var target = img.target;
  if (/^https?:\/\//i.test(target)) {
    callback(null, { url: target, auth: false });
    return;
  }
  var folder = s_path.split('/').slice(0, -1).join('/');
  if (!img.wiki) {
    var path = decodeURIComponentSafe(target.split(/[?#]/)[0]);
    path = path.charAt(0) === '/' ? path.slice(1) : (folder ? folder + '/' : '') + path;
    callback(null, assetUrl(normalizePath(path)));
    return;
  }
  function find(list) {
    var name = target.toLowerCase();
    var base = name.split('/').pop();
    var best = null;
    (list || []).forEach(function (a) {
      var p = (a.path || '').toLowerCase();
      if (p === name || p.split('/').pop() === base) {
        var near = p.split('/').slice(0, -1).join('/') === folder.toLowerCase();
        if (p === name || !best || (near && !best.near) ||
            (near === best.near && p.length < best.path.length)) {
          best = { path: a.path, near: near || p === name };
        }
      }
    });
    callback(null, assetUrl(best ? best.path : target));
  }
  if (s_assets) {
    find(s_assets);
    return;
  }
  apiGet('/api/assets', function (err, list) {
    if (!err) {
      s_assets = list;
    }
    find(list);
  });
}

function fetchBinary(source, callback) {
  var done = false;
  function finish(err, data, type) {
    if (!done) {
      done = true;
      clearTimeout(timer);
      callback(err, data, type);
    }
  }
  var xhr = new XMLHttpRequest();
  var timer = setTimeout(function () {
    try { xhr.abort(); } catch (e) { /* ignore */ }
    finish(t('timeout'));
  }, 60000);
  xhr.open('GET', source.url, true);
  if (source.auth) {
    xhr.setRequestHeader('Authorization', 'Bearer ' + loadSettings().token);
  }
  xhr.responseType = 'arraybuffer';
  xhr.onload = function () {
    if (xhr.status === 404) {
      finish(t('imageNotFound'));
    } else if (xhr.status < 200 || xhr.status >= 300) {
      finish(t('serverError', xhr.status));
    } else if (!xhr.response) {
      finish(t('imageFailed'));
    } else if (xhr.response.byteLength > IMAGE_MAX_BYTES) {
      finish(t('imageTooLarge'));
    } else {
      finish(null, xhr.response, xhr.getResponseHeader('Content-Type') || '');
    }
  };
  xhr.onerror = function () {
    finish(t('offline'));
  };
  xhr.send();
}

function isSvg(data, type) {
  if (/svg/i.test(type)) {
    return true;
  }
  var head = String.fromCharCode.apply(null, new Uint8Array(data.slice(0, 200)));
  return /<svg|<\?xml/i.test(head);
}

// callback(err, drawable, width, height)
function decodeImage(data, type, callback) {
  if (typeof createImageBitmap === 'undefined' || typeof OffscreenCanvas === 'undefined') {
    callback(t('imageFailed'));
    return;
  }
  if (isSvg(data, type)) {
    var url = URL.createObjectURL(new Blob([data], { type: 'image/svg+xml' }));
    var img = new Image();
    img.onload = function () {
      callback(null, img, img.naturalWidth || 200, img.naturalHeight || 200);
      URL.revokeObjectURL(url);
    };
    img.onerror = function () {
      URL.revokeObjectURL(url);
      callback(t('imageFailed'));
    };
    img.src = url;
    return;
  }
  createImageBitmap(new Blob([data], { type: type })).then(function (bmp) {
    callback(null, bmp, bmp.width, bmp.height);
  }, function () {
    callback(t('imageFailed'));
  });
}

// Scale to fit the box (halving in steps when shrinking a lot, which keeps
// photos from turning to noise), on the theme's background.
function fitImage(src, w, h, box, background) {
  var scale = Math.min(box.w / w, box.h / h, 3);
  var dw = Math.max(1, Math.round(w * scale));
  var dh = Math.max(1, Math.round(h * scale));
  var cur = src;
  var cw = w;
  var ch = h;
  while (cw / 2 >= dw * 2) {
    var half = new OffscreenCanvas(Math.round(cw / 2), Math.round(ch / 2));
    var hg = half.getContext('2d');
    hg.imageSmoothingQuality = 'high';
    hg.drawImage(cur, 0, 0, half.width, half.height);
    cur = half;
    cw = half.width;
    ch = half.height;
  }
  var canvas = new OffscreenCanvas(dw, dh);
  var g = canvas.getContext('2d');
  g.fillStyle = background;
  g.fillRect(0, 0, dw, dh);
  g.imageSmoothingEnabled = scale < 1;  // keep small icons crisp when enlarged
  g.imageSmoothingQuality = 'high';
  g.drawImage(cur, 0, 0, dw, dh);
  return { w: dw, h: dh, rgba: g.getImageData(0, 0, dw, dh).data };
}

// Floyd–Steinberg to two bits per channel: one GBitmapFormat8Bit byte a pixel.
function ditherTo64(img) {
  var w = img.w;
  var h = img.h;
  var src = img.rgba;
  var err = new Float32Array(w * h * 3);
  var out = new Array(w * h);
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      var i = y * w + x;
      var px = 0xc0;
      for (var c = 0; c < 3; c++) {
        var v = src[i * 4 + c] + err[i * 3 + c];
        var q = Math.max(0, Math.min(3, Math.round(v / 85)));
        var e = v - q * 85;
        px |= q << (4 - c * 2);
        if (x + 1 < w) err[(i + 1) * 3 + c] += e * 7 / 16;
        if (y + 1 < h) {
          if (x > 0) err[(i + w - 1) * 3 + c] += e * 3 / 16;
          err[(i + w) * 3 + c] += e * 5 / 16;
          if (x + 1 < w) err[(i + w + 1) * 3 + c] += e / 16;
        }
      }
      out[i] = px;
    }
  }
  return out;
}

function openImage(index) {
  var img = s_images[index];
  if (!img) {
    sendError(t('stale'));
    return;
  }
  imageSource(img, function (err, source) {
    if (err) {
      sendError(err);
      return;
    }
    fetchBinary(source, function (err, data, type) {
      if (err) {
        sendError(err);
        return;
      }
      decodeImage(data, type, function (err, drawable, w, h) {
        if (err) {
          sendError(err);
          return;
        }
        var background = loadSettings().theme === 'dark' ? '#000' : '#fff';
        var fitted;
        try {
          fitted = fitImage(drawable, w, h, imageBox(), background);
        } catch (e) {
          log('image: ' + e);
          sendError(t('imageFailed'));
          return;
        }
        sendBytes(KIND_IMAGE, ditherTo64(fitted), { LEVEL: fitted.w });
      });
    });
  });
}

Pebble.addEventListener('ready', function () {
  log('ready');
  sendDisplay(fetchList);
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (p.CMD === CMD_LIST) {
    fetchList();
  } else if (p.CMD === CMD_OPEN) {
    openEntry(p.LEVEL || 0, p.ARG || 0);
  } else if (p.CMD === CMD_SEARCH) {
    search(p.LEVEL || 0, p.TEXT || '');
  } else if (p.CMD === CMD_REFRESH) {
    refreshLevel(p.LEVEL || 0);
  } else if (p.CMD === CMD_PAGE) {
    sendPage(p.ARG || 0);
  } else if (p.CMD === CMD_LINK) {
    openLink(p.ARG || 0);
  } else if (p.CMD === CMD_BACK) {
    linkBack();
  } else if (p.CMD === CMD_IMAGE) {
    openImage(p.ARG || 0);
  }
});

// ---- Settings page ----
// A data: URI page, returned through pebblejs://close (same approach as
// coredevices/pebble-browser-app).

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// Same visual language as the watch: black ink on paper, section names on
// the black band, and a live preview of how notes look with the chosen theme
// and text size.
var CONFIG_CSS = [
  'body{margin:0;background:#fff;color:#000;font:16px/1.5 Roboto,"Noto Sans JP",sans-serif}',
  'main{max-width:480px;margin:0 auto;padding:0 16px 32px}',
  'h1{font-size:22px;margin:20px 0 2px}',
  '.lead{margin:0;color:#444;font-size:14px}',
  'h2{background:#000;color:#fff;font-size:14px;margin:24px -16px 4px;padding:3px 16px}',
  'label,.label{display:block;font-weight:700;margin:16px 0 6px}',
  '.hint{font-size:13px;color:#444;margin:6px 0 0}',
  '.field{width:100%;box-sizing:border-box;padding:10px 12px;font:inherit;border:2px solid #000;border-radius:6px;background:#fff;color:#000}',
  '.tok{display:flex;gap:8px}.tok .field{flex:1}',
  '.tok button{border:2px solid #000;background:#fff;color:#000;border-radius:6px;padding:0 14px;font:inherit;font-size:14px;font-weight:700}',
  '.seg{display:flex;border:2px solid #000;border-radius:6px;overflow:hidden}',
  '.seg label{flex:1;margin:0;text-align:center;position:relative}',
  '.seg label+label{border-left:2px solid #000}',
  '.seg input{position:absolute;opacity:0;inset:0;margin:0}',
  '.seg span{display:block;padding:9px}',
  '.seg input:checked+span{background:#000;color:#fff}',
  '.field:focus,button:focus-visible,.seg input:focus-visible+span{outline:3px solid #0055aa;outline-offset:2px}',
  // watch preview: 200x228 at 1:1, 6px side margins as on the watch
  '.pv{width:188px;height:228px;padding:0 6px;margin:16px auto 0;border:10px solid #2a2a2a;border-radius:16px;overflow:hidden;box-sizing:content-box;font-family:"sans-serif-condensed","Roboto Condensed","Arial Narrow",sans-serif;line-height:1.2}',
  '.pv.light{background:#fff;color:#000;--acc:#0055aa;--done:#00aa00;--rule:#555}',
  '.pv.dark{background:#000;color:#fff;--acc:#55aaff;--done:#55ff55;--rule:#aaa}',
  '.pv.large{--fs:24px;--ms:18px}.pv.normal{--fs:18px;--ms:14px}',
  '.pv .t{font-weight:700;font-size:24px;margin-top:2px}',
  '.pv .m{font-size:var(--ms);border-bottom:2px solid var(--rule);padding-bottom:4px}',
  '.pv .h{color:var(--acc);font-weight:700;font-size:24px;border-bottom:3px solid var(--acc);margin-top:8px}',
  '.pv .p{font-size:var(--fs);margin-top:2px;padding-left:1.1em;position:relative}',
  '.pv .p:before{content:"";position:absolute;left:2px;top:.45em;width:.42em;height:.42em}',
  '.pv .li:before{background:currentColor;border-radius:50%;top:.5em;width:.3em;height:.3em}',
  '.pv .open:before{border:2px solid currentColor;border-radius:2px;box-sizing:border-box}',
  '.pv .done:before{background:var(--done);border-radius:2px}',
  '.caption{text-align:center;font-size:13px;color:#444;margin:6px 0 0}',
  '.row{display:flex;gap:12px;align-items:center;margin-top:32px}',
  '#save{flex:1;background:#000;color:#fff;border:0;border-radius:6px;padding:14px;font:inherit;font-weight:700}',
  '#cancel{background:none;border:0;padding:14px 8px;font:inherit;color:#000;text-decoration:underline}'
].join('');

// Settings page text; every label carries both languages so switching the
// language control relabels the page in place.
var CONFIG_TEXT = {
  lead: ['時計に表示するノートの取得先と、見た目を設定します。',
    'Where the watch gets your notes, and how they look.'],
  connection: ['接続', 'Connection'],
  server: ['サーバーのURL', 'Server URL'],
  token: ['トークン', 'Token'],
  tokenHint: ['ZenNotesアプリに入れているものと同じトークン', 'The same token you use in the ZenNotes app'],
  show: ['表示', 'Show'],
  hide: ['隠す', 'Hide'],
  display: ['表示', 'Display'],
  folder: ['表示するフォルダ', 'Folder to show'],
  allFolders: ['すべてのフォルダ', 'All folders'],
  folderHint: ['例: inbox。空欄ならすべて表示します(ゴミ箱は除く)',
    'For example inbox. Leave empty to show everything except the trash'],
  language: ['言語', 'Language'],
  theme: ['テーマ', 'Theme'],
  dark: ['ダーク', 'Dark'],
  light: ['ライト', 'Light'],
  font: ['文字サイズ', 'Text size'],
  large: ['大', 'Large'],
  normal: ['標準', 'Normal'],
  status: ['ステータスバー', 'Status line'],
  scrollbar: ['スクロール位置の目盛り', 'Scroll position marker'],
  scroll: ['上下ボタン1回で進む量', 'One UP/DOWN press moves'],
  small: ['少し', 'A little'],
  half: ['半画面', 'Half'],
  page: ['1画面', 'A screen'],
  scrollHint: ['押し続けるとだんだん速くなります', 'Hold the button to speed up'],
  on: ['表示', 'On'],
  off: ['非表示', 'Off'],
  caption: ['時計での見え方(目安)', 'Preview on the watch (approximate)'],
  cancel: ['キャンセル', 'Cancel'],
  save: ['保存', 'Save']
};

var CONFIG_JS = [
  'var TEXT=' + JSON.stringify(CONFIG_TEXT) + ';',
  'function $(id){return document.getElementById(id);}',
  'function pick(n){return document.querySelector("input[name="+n+"]:checked").value;}',
  'function lang(){return pick("lang")==="en"?1:0;}',
  'function relabel(){var l=lang();var els=document.querySelectorAll("[data-t]");',
  'for(var i=0;i<els.length;i++){els[i].textContent=TEXT[els[i].getAttribute("data-t")][l];}',
  '$("folder").placeholder=TEXT.allFolders[l];',
  '$("show").textContent=TEXT[$("token").type==="text"?"hide":"show"][l];',
  'document.documentElement.lang=l?"en":"ja";}',
  'function render(){$("pv").className="pv "+pick("theme")+" "+pick("font")+',
  '" status-"+pick("status")+" bar-"+pick("scrollbar");relabel();}',
  'var radios=document.querySelectorAll("input[type=radio]");',
  'for(var i=0;i<radios.length;i++){radios[i].onchange=render;}',
  'render();',
  '$("show").onclick=function(){var t=$("token");t.type=t.type==="text"?"password":"text";relabel();};',
  'function close(v){location.href="pebblejs://close#"+encodeURIComponent(JSON.stringify(v));}',
  'function val(id){return $(id).value.trim();}',
  '$("cancel").onclick=function(){close({});};',
  '$("save").onclick=function(){close({server:val("server"),token:val("token"),',
  'folder:val("folder"),theme:pick("theme"),font:pick("font"),status:pick("status"),',
  'scrollbar:pick("scrollbar"),lang:pick("lang"),scroll:pick("scroll")});};'
].join('');

var CONFIG_PREVIEW_CSS = [
  '.pv{position:relative}',
  '.pv .sl{position:absolute;left:0;right:0;bottom:0;height:16px;border-top:1px solid currentColor;font:bold 12px/15px sans-serif;display:none;background:inherit}',
  '.pv.status-on .sl{display:flex;gap:6px}',
  '.pv .sl b{background:currentColor;padding:0 4px}',
  '.pv.light .sl b{color:#fff;background:#000}.pv.dark .sl b{color:#000;background:#fff}',
  '.pv .sl i{font-style:normal;margin-left:auto;padding-right:3px}',
  '.pv .sb{position:absolute;right:0;top:10px;width:3px;height:60px;background:var(--rule);display:none}',
  '.pv.bar-on .sb{display:block}'
].join('');

function configPage() {
  var cfg = loadSettings();
  var l = cfg.lang === 'en' ? 1 : 0;
  function tx(key, tag, attrs) {
    tag = tag || 'span';
    return '<' + tag + (attrs ? ' ' + attrs : '') + ' data-t=' + key + '>' +
      CONFIG_TEXT[key][l] + '</' + tag + '>';
  }
  function seg(name, options) {
    return '<div class=seg role=radiogroup>' + options.map(function (o) {
      // o = [value, text key or literal]
      var label = CONFIG_TEXT[o[1]] ? tx(o[1]) : '<span>' + o[1] + '</span>';
      return '<label><input type=radio name=' + name + ' value=' + o[0] +
        (cfg[name] === o[0] ? ' checked' : '') + '>' + label +
        '</label>';
    }).join('') + '</div>';
  }
  var html =
    '<!doctype html><html lang=' + (l ? 'en' : 'ja') + '><meta charset=utf-8>' +
    '<meta name=viewport content="width=device-width,initial-scale=1">' +
    '<title>ZenNotes for Pebble</title><style>' + CONFIG_CSS + CONFIG_PREVIEW_CSS + '</style><main>' +
    '<h1>ZenNotes for Pebble</h1>' +
    tx('lead', 'p', 'class=lead') +

    tx('connection', 'h2') +
    tx('server', 'label', 'for=server') +
    '<input id=server class=field type=url autocapitalize=off autocorrect=off ' +
    'placeholder="https://zennotes.example.com" value="' +
    esc(cfg.server) + '">' +
    tx('token', 'label', 'for=token') +
    '<div class=tok><input id=token class=field type=password autocapitalize=off ' +
    'autocorrect=off value="' + esc(cfg.token) + '"><button id=show type=button>' +
    CONFIG_TEXT.show[l] + '</button></div>' +
    tx('tokenHint', 'p', 'class=hint') +

    tx('display', 'h2') +
    tx('language', 'span', 'class=label') + seg('lang', [['ja', '日本語'], ['en', 'English']]) +
    tx('folder', 'label', 'for=folder') +
    '<input id=folder class=field autocapitalize=off autocorrect=off placeholder="' +
    CONFIG_TEXT.allFolders[l] + '" value="' + esc(cfg.folder) + '">' +
    tx('folderHint', 'p', 'class=hint') +
    tx('theme', 'span', 'class=label') + seg('theme', [['dark', 'dark'], ['light', 'light']]) +
    tx('font', 'span', 'class=label') + seg('font', [['large', 'large'], ['normal', 'normal']]) +
    tx('status', 'span', 'class=label') + seg('status', [['on', 'on'], ['off', 'off']]) +
    tx('scrollbar', 'span', 'class=label') + seg('scrollbar', [['on', 'on'], ['off', 'off']]) +
    tx('scroll', 'span', 'class=label') + seg('scroll', [['small', 'small'], ['half', 'half'], ['page', 'page']]) +
    tx('scrollHint', 'p', 'class=hint') +
    '<div id=pv class=pv aria-hidden=true>' +
    '<div class=t>' + (l ? 'Camping gear' : 'キャンプの持ち物') + '</div>' +
    '<div class=m>camp  2026/10/09</div>' +
    '<div class=h>' + (l ? 'Tent' : 'テント') + '</div>' +
    '<div class="p li">' + (l ? '12 stakes' : 'ペグ 12本') + '</div>' +
    '<div class="p done">' + (l ? 'Tarp' : 'タープ') + '</div>' +
    '<div class="p open">' + (l ? 'Lantern' : 'ランタン') + '</div>' +
    '<div class=sb></div>' +
    // Status line hints are English on the watch in both languages.
    '<div class=sl><b>NOTE</b><span>◀ back</span><i>Top</i></div>' +
    '</div>' +
    tx('caption', 'p', 'class=caption') +

    '<div class=row>' + tx('cancel', 'button', 'id=cancel type=button') +
    tx('save', 'button', 'id=save type=button') + '</div>' +
    '</main><script>' + CONFIG_JS + '<\/script>';
  return 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
}

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(configPage());
});

Pebble.addEventListener('webviewclosed', function (e) {
  var r = e && e.response;
  if (!r) {
    return;
  }
  var cfg;
  try {
    cfg = JSON.parse(decodeURIComponent(r));
  } catch (err) {
    try { cfg = JSON.parse(r); } catch (err2) { log('bad config response'); return; }
  }
  if (!cfg || cfg.server === undefined) {
    return;
  }
  saveSettings(cfg);
  log('settings saved');
  sendDisplay(fetchList);
});
