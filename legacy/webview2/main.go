package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/jchv/go-webview2"
	"github.com/yuin/goldmark"
	"github.com/yuin/goldmark/extension"
	"github.com/yuin/goldmark/parser"
	"github.com/yuin/goldmark/renderer/html"
)

type prefs struct {
	Dark  bool    `json:"dark"`
	Split float64 `json:"split"`
}

func prefsPath() string {
	dir, _ := os.UserConfigDir()
	return filepath.Join(dir, "fmdv", "prefs.json")
}

func loadPrefs() prefs {
	p := prefs{Split: 50}
	data, err := os.ReadFile(prefsPath())
	if err == nil {
		json.Unmarshal(data, &p)
	}
	return p
}

func savePrefs(p prefs) {
	path := prefsPath()
	os.MkdirAll(filepath.Dir(path), 0755)
	data, _ := json.Marshal(p)
	os.WriteFile(path, data, 0644)
}

var md = goldmark.New(
	goldmark.WithExtensions(extension.GFM, extension.Table, extension.Strikethrough, extension.TaskList),
	goldmark.WithParserOptions(parser.WithAutoHeadingID()),
	goldmark.WithRendererOptions(html.WithUnsafe()),
)

func main() {
	if len(os.Args) < 2 {
		os.Exit(1)
	}
	mdPath := os.Args[1]

	type result struct {
		raw      []byte
		rendered string
		err      error
	}
	ch := make(chan result, 1)
	go func() {
		data, err := os.ReadFile(mdPath)
		if err != nil {
			ch <- result{err: err}
			return
		}
		// normalize line endings before goldmark and before embedding as JS string
		data = bytes.ReplaceAll(data, []byte("\r\n"), []byte("\n"))
		var buf bytes.Buffer
		md.Convert(data, &buf)
		ch <- result{raw: data, rendered: buf.String()}
	}()

	w := webview2.NewWithOptions(webview2.WebViewOptions{Debug: false})
	if w == nil {
		os.Exit(1)
	}
	defer w.Destroy()

	r := <-ch
	if r.err != nil {
		os.Exit(1)
	}

	title := strings.TrimSuffix(filepath.Base(mdPath), filepath.Ext(mdPath))
	w.SetTitle(title + " — FMDV")
	w.SetSize(1100, 800, webview2.HintNone)

	p := loadPrefs()

	// Expose saveFile(content) to JS for Ctrl+S
	w.Bind("saveFile", func(content string) {
		os.WriteFile(mdPath, []byte(content), 0644)
	})

	// Expose savePref(key, value) to JS so preferences survive across launches
	w.Bind("savePref", func(key string, value interface{}) {
		switch key {
		case "dark":
			p.Dark = value.(bool)
		case "split":
			p.Split = value.(float64)
		}
		savePrefs(p)
	})

	rawJSON, _ := json.Marshal(string(r.raw))
	w.SetHtml(buildPage(title, r.rendered, string(rawJSON), p))
	w.Run()
}

// buildPage produces HTML using Go string concatenation (no fmt.Sprintf)
// so we never fight with % escaping or backtick raw-string conflicts.
func buildPage(title, renderedHTML, rawJSON string, p prefs) string {
	darkInit := "false"
	if p.Dark {
		darkInit = "true"
	}
	split := p.Split
	if split <= 0 {
		split = 50
	}
	splitInit := strconv.FormatFloat(split, 'f', 2, 64)
	// rawJSON is already a valid JSON string literal (with quotes), safe to embed in JS.
	// renderedHTML is trusted HTML from goldmark — shown immediately, no JS needed.
	return `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>` + title + `</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}

body{
  --bg:#fff;--bg2:#f6f8fa;--bg3:#fafbfc;
  --border:#d0d7de;--text:#24292f;--text2:#57606a;
  --link:#0969da;--pre-bg:#f6f8fa;
  --ob:rgba(246,248,250,.92);--obd:rgba(175,184,193,.5);
  background:var(--bg);color:var(--text)
}
body.dark{
  --bg:#0d1117;--bg2:#161b22;--bg3:#161b22;
  --border:#30363d;--text:#e6edf3;--text2:#8b949e;
  --link:#58a6ff;--pre-bg:#161b22;
  --ob:rgba(22,27,34,.92);--obd:rgba(48,54,61,.8)
}

#main{display:flex;overflow:hidden;height:100%}
#editor{display:none;width:50%;border-right:1px solid var(--border);flex-direction:column;flex-shrink:0;position:relative}
#editor textarea{width:100%;height:100%;padding:20px;font-family:"SFMono-Regular",Consolas,monospace;font-size:14px;line-height:1.6;border:none;outline:none;resize:none;color:var(--text);background:var(--bg3)}
#divider{width:4px;background:var(--border);cursor:col-resize;flex-shrink:0;display:none}
#divider:hover,#divider.drag{background:var(--link)}
#preview{flex:1;overflow-y:auto;padding:32px 40px 80px;color:var(--text);line-height:1.6;font-size:16px;background:var(--bg)}

.fb{position:absolute;width:26px;height:26px;border-radius:5px;background:var(--ob);border:1px solid var(--obd);cursor:pointer;display:flex;align-items:center;justify-content:center;z-index:20;padding:0}
.fb:hover{background:var(--bg2)}
#fb-done{top:8px;right:20px}
#fb-dark{top:40px;right:20px}
#fb-dark.on svg path{stroke:var(--link)}

#preview h1,#preview h2,#preview h3,#preview h4,#preview h5,#preview h6{margin-top:24px;margin-bottom:16px;font-weight:600;line-height:1.25}
#preview h1{font-size:2em;border-bottom:1px solid var(--border);padding-bottom:.3em}
#preview h2{font-size:1.5em;border-bottom:1px solid var(--border);padding-bottom:.3em}
#preview h3{font-size:1.25em}
#preview a{color:var(--link);text-decoration:none}
#preview a:hover{text-decoration:underline}
#preview code{font-family:"SFMono-Regular",Consolas,monospace;font-size:.85em;background:var(--pre-bg);border-radius:6px;padding:.2em .4em}
#preview pre{background:var(--pre-bg);border-radius:6px;padding:16px;overflow:auto;line-height:1.45;margin:16px 0;border:1px solid var(--border)}
#preview pre code{background:none;padding:0;font-size:1em}
#preview blockquote{padding:0 1em;color:var(--text2);border-left:4px solid var(--border);margin:16px 0}
#preview table{border-collapse:collapse;width:100%;margin:16px 0}
#preview th,#preview td{border:1px solid var(--border);padding:6px 13px}
#preview th{background:var(--bg2);font-weight:600}
#preview tr:nth-child(even) td{background:var(--bg2)}
#preview img{max-width:100%}
#preview hr{border:none;border-top:1px solid var(--border);margin:24px 0}
#preview ul,#preview ol{padding-left:2em;margin:8px 0}
#preview li+li{margin-top:.25em}
#preview p{margin:16px 0}
#preview p:first-child{margin-top:0}
#preview input[type=checkbox]{margin-right:.5em}
#preview strong{font-weight:600}
</style>
</head>
<body>
<div id="main">
  <div id="editor">
    <textarea id="ta" spellcheck="false"></textarea>
    <button id="fb-done" class="fb" onclick="closeEdit()" title="Done (Ctrl+E)">
      <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
        <path d="M2 7L5.5 10.5L12 3.5" stroke="var(--text2)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
      </svg>
    </button>
    <button id="fb-dark" class="fb" onclick="toggleDark()" title="Dark mode (Ctrl+D)">
      <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
        <path d="M7 1.5a5.5 5.5 0 1 0 5.5 5.5A5.5 5.5 0 0 1 7 1.5z" stroke="var(--text2)" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>
      </svg>
    </button>
  </div>
  <div id="divider"></div>
  <div id="preview">` + renderedHTML + `</div>
</div>
<script>
// raw source for live edit — JSON string, safely embedded
var raw = ` + rawJSON + `;

var preview=document.getElementById('preview');
var ta=document.getElementById('ta');
var editor=document.getElementById('editor');
var divider=document.getElementById('divider');
var editing=false;
var dark=` + darkInit + `;
var splitPct=` + splitInit + `;

if(dark){document.body.classList.add('dark');document.getElementById('fb-dark').classList.add('on');}
ta.value=raw;

function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}

function renderMD(text){
  text=text.replace(/\r\n/g,'\n').replace(/\r/g,'\n');
  var h=text
    .replace(/\x60\x60\x60(\w*)\n([\s\S]*?)\x60\x60\x60/gm,function(_,l,c){return '<pre><code>'+esc(c.trimEnd())+'</code></pre>';})
    .replace(/\x60([^\x60\n]+)\x60/g,function(_,c){return '<code>'+esc(c)+'</code>';})
    .replace(/^(#{1,6}) (.+)$/gm,function(_,h,t){var n=h.length;return '<h'+n+'>'+t+'</h'+n+'>';})
    .replace(/^> (.+)$/gm,'<blockquote>$1</blockquote>')
    .replace(/^---+$/gm,'<hr>')
    .replace(/\*\*\*(.+?)\*\*\*/g,'<strong><em>$1</em></strong>')
    .replace(/\*\*(.+?)\*\*/g,'<strong>$1</strong>')
    .replace(/\*([^*\n]+)\*/g,'<em>$1</em>')
    .replace(/~~(.+?)~~/g,'<del>$1</del>')
    .replace(/^- \[x\] (.+)$/gm,'<li><input type="checkbox" checked disabled> $1</li>')
    .replace(/^- \[ \] (.+)$/gm,'<li><input type="checkbox" disabled> $1</li>')
    .replace(/^[*-] (.+)$/gm,'<li>$1</li>')
    .replace(/^\d+\. (.+)$/gm,'<oli>$1</oli>')
    .replace(/!\[([^\]]*)\]\(([^)]+)\)/g,'<img alt="$1" src="$2">')
    .replace(/\[([^\]]+)\]\(([^)]+)\)/g,'<a href="$2" target="_blank">$1</a>');
  return h.split(/\n{2,}/).map(function(b){
    b=b.trim();if(!b)return '';
    if(/^<(h[1-6]|pre|blockquote|hr|table|img)/i.test(b))return b;
    if(/<li/.test(b)){
      b=b.replace(/(<li[\s\S]*?<\/li>)+/g,function(m){return '<ul>'+m+'</ul>';});
      b=b.replace(/(<oli>[\s\S]*?<\/oli>)+/g,function(m){return '<ol>'+m.replace(/<\/?oli>/g,function(t){return t==='<oli>'?'<li>':'</li>';})+'</ol>';});
      return b;
    }
    return '<p>'+b.replace(/\n/g,' ')+'</p>';
  }).join('\n');
}

ta.addEventListener('input',function(){preview.innerHTML=renderMD(ta.value);});

function openEdit(){
  editing=true;
  editor.style.display='flex';
  editor.style.width=splitPct+'%';
  divider.style.display='block';
  ta.focus();
}
function closeEdit(){
  editing=false;
  editor.style.display='none';
  divider.style.display='none';
}
function toggleDark(){
  dark=!dark;
  document.body.classList.toggle('dark',dark);
  document.getElementById('fb-dark').classList.toggle('on',dark);
  savePref('dark',dark);
}

document.addEventListener('keydown',function(e){
  if(e.ctrlKey&&!e.altKey){
    if(e.key==='e'||e.key==='E'){e.preventDefault();editing?closeEdit():openEdit();}
    if(e.key==='d'||e.key==='D'){e.preventDefault();toggleDark();}
    if((e.key==='s'||e.key==='S')&&editing){
      e.preventDefault();
      saveFile(ta.value);
      if(e.shiftKey){closeEdit();}
    }
  }
});

var dragging=false;
divider.addEventListener('mousedown',function(e){dragging=true;divider.classList.add('drag');e.preventDefault();});
document.addEventListener('mousemove',function(e){
  if(!dragging)return;
  var r=document.getElementById('main').getBoundingClientRect();
  splitPct=Math.max(20,Math.min(80,(e.clientX-r.left)/r.width*100));
  editor.style.width=splitPct+'%';
});
document.addEventListener('mouseup',function(){
  if(dragging){dragging=false;divider.classList.remove('drag');savePref('split',splitPct);}
});
</script>
</body>
</html>`
}
