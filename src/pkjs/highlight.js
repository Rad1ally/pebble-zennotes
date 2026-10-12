// Small syntax highlighter for code blocks shown on the watch.
//
// Each output line carries markers: "\x01" followed by a class character
// switches the colour from there on (k keyword, s string, n number/constant/
// variable, c comment, p plain). The watch draws the runs in its theme colours.
// It is a tokenizer, not a parser: good enough to make code readable at a
// glance, and anything it doesn't recognise stays plain.

var C_LIKE_KW = 'auto break case catch char class const continue default do double else enum ' +
  'extern final finally float for goto if implements import inline int interface long new ' +
  'package private protected public register return short signed sizeof static struct super ' +
  'switch this throw throws try typedef union unsigned using virtual void volatile while ' +
  'namespace template typename bool fn let mut impl trait pub use mod match loop where self ' +
  'func go defer chan map range select type var abstract override sealed async await';

var LANGS = {
  javascript: {
    name: 'JavaScript', line: ['//'], block: [['/*', '*/']], quotes: '"\'`',
    kw: 'async await break case catch class const continue debugger default delete do else ' +
      'export extends finally for from function if import in instanceof let new of return ' +
      'static super switch this throw try typeof var void while with yield',
    consts: 'true false null undefined NaN Infinity'
  },
  typescript: {
    name: 'TypeScript', line: ['//'], block: [['/*', '*/']], quotes: '"\'`',
    kw: 'abstract as async await break case catch class const continue declare default do ' +
      'else enum export extends finally for from function if implements import in ' +
      'instanceof interface keyof let namespace new of private protected public readonly ' +
      'return static super switch this throw try type typeof var void while yield',
    consts: 'true false null undefined NaN Infinity'
  },
  python: {
    name: 'Python', line: ['#'], block: [['"""', '"""'], ["'''", "'''"]], quotes: '"\'',
    kw: 'and as assert async await break class continue def del elif else except finally ' +
      'for from global if import in is lambda nonlocal not or pass raise return try while ' +
      'with yield match case',
    consts: 'True False None self'
  },
  shell: {
    name: 'Shell', line: ['#'], block: [], quotes: '"\'', vars: /^\$(\{[^}]*\}|\w+|[@#?$!*0-9])/,
    kw: 'if then else elif fi for while until do done case esac in function return local ' +
      'export readonly select time break continue source alias unset set echo cd exit sudo',
    consts: 'true false'
  },
  json: {
    name: 'JSON', line: [], block: [], quotes: '"', kw: '', consts: 'true false null'
  },
  yaml: {
    name: 'YAML', line: ['#'], block: [], quotes: '"\'', keys: /^(\s*-?\s*)([\w.\-\/]+)(\s*:)(?=\s|$)/,
    kw: '', consts: 'true false null yes no on off ~'
  },
  css: {
    name: 'CSS', line: [], block: [['/*', '*/']], quotes: '"\'', keys: /^(\s*)([\w-]+)(\s*:)(?!:)/,
    kw: '@media @import @font-face @keyframes @supports @layer !important',
    consts: ''
  },
  html: {
    name: 'HTML', line: [], block: [['<!--', '-->']], quotes: '"\'', markup: true,
    kw: '', consts: ''
  },
  sql: {
    name: 'SQL', line: ['--'], block: [['/*', '*/']], quotes: '\'"', ci: true,
    kw: 'select from where and or not insert into values update set delete create table ' +
      'alter drop index view join left right inner outer full on as group by order having ' +
      'limit offset distinct union all case when then else end is in like between exists ' +
      'primary key foreign references default unique with returning asc desc',
    consts: 'null true false'
  },
  c: { name: 'C', line: ['//'], block: [['/*', '*/']], quotes: '"\'', kw: C_LIKE_KW, consts: 'true false NULL nullptr' },
  php: {
    name: 'PHP', line: ['//', '#'], block: [['/*', '*/']], quotes: '"\'', vars: /^\$\w+/,
    kw: 'abstract and array as break case catch class clone const continue declare default ' +
      'do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile ' +
      'extends final finally fn for foreach function global if implements include ' +
      'include_once instanceof insteadof interface isset list match namespace new or print ' +
      'private protected public readonly require require_once return static switch throw ' +
      'trait try unset use var while xor yield',
    consts: 'true false null TRUE FALSE NULL'
  },
  ruby: {
    name: 'Ruby', line: ['#'], block: [['=begin', '=end']], quotes: '"\'', vars: /^(@@?\w+|:\w+[?!]?|\$\w+)/,
    kw: 'alias and begin break case class def defined? do else elsif end ensure for if in ' +
      'module next not or redo rescue retry return self super then undef unless until when ' +
      'while yield require attr_accessor attr_reader attr_writer puts',
    consts: 'true false nil'
  }
};

var ALIASES = {
  js: 'javascript', jsx: 'javascript', mjs: 'javascript', cjs: 'javascript', node: 'javascript',
  ts: 'typescript', tsx: 'typescript',
  py: 'python', python3: 'python',
  sh: 'shell', bash: 'shell', zsh: 'shell', console: 'shell', shellscript: 'shell',
  yml: 'yaml', scss: 'css', less: 'css',
  xml: 'html', svg: 'html', vue: 'html',
  h: 'c', cpp: 'c', 'c++': 'c', cc: 'c', hpp: 'c', java: 'c', go: 'c', golang: 'c',
  rust: 'c', rs: 'c', cs: 'c', csharp: 'c', kotlin: 'c', kt: 'c', swift: 'c',
  rb: 'ruby'
};

// Names shown on the watch for languages that share a highlighter.
var DISPLAY = {
  cpp: 'C++', 'c++': 'C++', cc: 'C++', hpp: 'C++', java: 'Java', go: 'Go', golang: 'Go',
  rust: 'Rust', rs: 'Rust', cs: 'C#', csharp: 'C#', kotlin: 'Kotlin', kt: 'Kotlin',
  swift: 'Swift', xml: 'XML', svg: 'SVG', vue: 'Vue', scss: 'SCSS', less: 'Less',
  txt: 'Text', text: 'Text', plain: 'Text', plaintext: 'Text', md: 'Markdown', markdown: 'Markdown',
  dockerfile: 'Dockerfile', ini: 'INI', toml: 'TOML', diff: 'Diff'
};

function words(list) {
  var set = {};
  list.split(/\s+/).forEach(function (w) { if (w) { set[w] = true; } });
  return set;
}

for (var id in LANGS) {
  LANGS[id].kwSet = words(LANGS[id].kw);
  LANGS[id].constSet = words(LANGS[id].consts);
}

function resolve(lang) {
  var key = (lang || '').toLowerCase();
  var id = LANGS.hasOwnProperty(key) ? key : ALIASES[key];
  return { spec: id ? LANGS[id] : null, name: DISPLAY[key] || (id ? LANGS[id].name : lang || '') };
}

function startsAt(line, i, token) {
  return line.substr(i, token.length) === token;
}

// Returns marked-up lines for the given code lines.
function highlight(lang, lines) {
  var spec = resolve(lang).spec;
  if (!spec) {
    return lines;
  }
  var openBlock = null;  // closing token of a block comment / string still open

  return lines.map(function (line) {
    var out = '';
    var cls = 'p';
    function emit(c, text) {
      if (!text) {
        return;
      }
      if (c !== cls) {
        out += '\x01' + c;
        cls = c;
      }
      out += text;
    }

    var i = 0;
    if (openBlock) {
      var end = line.indexOf(openBlock.close);
      if (end < 0) {
        emit(openBlock.cls, line);
        return out;
      }
      emit(openBlock.cls, line.slice(0, end + openBlock.close.length));
      i = end + openBlock.close.length;
      openBlock = null;
    }
    if (spec.keys && i === 0) {
      var km = line.match(spec.keys);
      if (km) {
        emit('p', km[1]);
        emit('k', km[2]);
        emit('p', km[3]);
        i = km[0].length;
      }
    }

    while (i < line.length) {
      var rest = line.slice(i);
      var ch = line.charAt(i);
      var m;
      var j;

      // Block comments and block strings (Python triple quotes, Ruby =begin).
      var block = null;
      for (j = 0; j < spec.block.length; j++) {
        if (startsAt(line, i, spec.block[j][0])) {
          block = spec.block[j];
          break;
        }
      }
      if (block) {
        var cls2 = block[0].charAt(0) === '"' || block[0].charAt(0) === "'" ? 's' : 'c';
        var close = line.indexOf(block[1], i + block[0].length);
        if (close < 0) {
          emit(cls2, rest);
          openBlock = { close: block[1], cls: cls2 };
          break;
        }
        emit(cls2, line.slice(i, close + block[1].length));
        i = close + block[1].length;
        continue;
      }
      var isLine = false;
      for (j = 0; j < spec.line.length; j++) {
        // "#" in shell only starts a comment at a word boundary ($#, a#b are not).
        if (startsAt(line, i, spec.line[j]) &&
            (spec.line[j] !== '#' || i === 0 || /[\s;]/.test(line.charAt(i - 1)))) {
          isLine = true;
          break;
        }
      }
      if (isLine) {
        emit('c', rest);
        break;
      }
      if (spec.quotes.indexOf(ch) >= 0) {
        j = i + 1;
        while (j < line.length && line.charAt(j) !== ch) {
          j += line.charAt(j) === '\\' ? 2 : 1;
        }
        emit('s', line.slice(i, j + 1));
        i = j + 1;
        continue;
      }
      if (spec.markup && ch === '<' && (m = rest.match(/^<\/?[\w:-]+/))) {
        emit('k', m[0]);
        i += m[0].length;
        continue;
      }
      if (spec.vars && (m = rest.match(spec.vars))) {
        emit('n', m[0]);
        i += m[0].length;
        continue;
      }
      if ((m = rest.match(/^(#[0-9a-fA-F]{3,8}\b|0x[0-9a-fA-F]+|\d[\d_]*(\.\d+)?([eE][+-]?\d+)?[a-z%]*)/)) &&
          (i === 0 || !/[\w$]/.test(line.charAt(i - 1)))) {
        emit('n', m[0]);
        i += m[0].length;
        continue;
      }
      if ((m = rest.match(/^[@!]?[A-Za-z_][\w-]*[?!]?/))) {
        var word = m[0];
        // Hyphens only belong to words in CSS/HTML-ish languages.
        if (!spec.keys && !spec.markup) {
          word = word.replace(/-.*$/, '');
        }
        var key = spec.ci ? word.toLowerCase() : word;
        if (spec.kwSet[key]) {
          emit('k', word);
        } else if (spec.constSet[key]) {
          emit('n', word);
        } else {
          emit('p', word);
        }
        i += word.length;
        continue;
      }
      emit('p', ch);
      i++;
    }
    return out;
  });
}

module.exports = { highlight: highlight, languageName: function (lang) { return resolve(lang).name; } };
