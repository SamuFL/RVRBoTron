(function () {
  "use strict";

  var token = new URLSearchParams(window.location.search).get("token") || "";
  var sourceInput = document.getElementById("source");
  var sourceName = document.getElementById("source-name");
  var templateSelect = document.getElementById("template");
  var renderButton = document.getElementById("render");
  var formatButton = document.getElementById("format");
  var downloadRequestButton = document.getElementById("download-request");
  var statusBox = document.getElementById("status");
  var factsBox = document.getElementById("facts");
  var player = document.getElementById("player");
  var download = document.getElementById("download");

  // The JSON editing surface (issue #138). Ace text APIs only -- no HTML
  // annotations, tooltips, or completion markup: request text and renderer
  // diagnostics are untrusted content, and the one status panel is where
  // they are shown, as text. Workers are disabled before the mode is
  // attached, not left to the Content-Security-Policy to block; the
  // renderer remains the sole validator.
  var editor = ace.edit("request-editor");
  editor.session.setOption("useWorker", false);
  editor.session.setMode("ace/mode/json");
  editor.setTheme("ace/theme/tomorrow_night");
  editor.setOptions({
    fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace",
    fontSize: "13px",
    showPrintMargin: false,
    wrap: false
  });

  // The editor's current string is the one request source of truth, for
  // both Render and Download -- never a separate parsed/re-serialized copy.
  function getRequestText() {
    return editor.getValue();
  }

  function setRequestText(text) {
    editor.setValue(text, -1); // -1: cursor at the start, nothing selected
  }

  function api(path) {
    return path + "?token=" + encodeURIComponent(token);
  }

  // The four committed Request templates (issue #140), executable
  // documentation rendered through the real renderer in tests -- discover
  // the editable structure here, not from memory. Labels match the option
  // text exactly, for status messages and the confirmation prompt.
  var TEMPLATE_LABELS = {
    simple: "Simple",
    full: "Full",
    modulated: "Modulated",
    spatial: "Spatial"
  };

  // The two checkpoints a template load never needs confirmation against:
  // whatever was last loaded, and whatever last rendered successfully.
  // Editing away from both, then trying to switch templates, is the one
  // case that can lose work -- and the only one that asks first.
  var lastLoadedTemplateText = null;
  var lastRenderedText = null;
  var activeTemplateKey = "simple";

  // Bumped by every loadTemplate call, and captured per call as
  // requestId: a fetch whose id no longer matches this counter when it
  // resolves has been superseded by a later load (the user switching
  // again before the first fetch settles, or startup's own load losing
  // to an early click) and applies nothing, so an out-of-order response
  // can never silently overwrite a more recent selection.
  var templateRequestId = 0;

  function fetchTemplateText(key) {
    return fetch(api("/templates/" + key + ".json")).then(function (response) {
      if (!response.ok) {
        throw new Error("could not load the " + TEMPLATE_LABELS[key] + " template");
      }
      return response.text();
    });
  }

  function applyTemplate(key, text) {
    setRequestText(text);
    lastLoadedTemplateText = text;
    activeTemplateKey = key;
    templateSelect.value = key;
  }

  function loadTemplate(key, loadedMessage) {
    templateRequestId += 1;
    var requestId = templateRequestId;
    // The confirmation prompt (or its absence, when nothing is at risk)
    // only accounts for edits that already existed when this load began.
    // Typing during the fetch itself -- brief, but real on a slow
    // loopback connection or a large template -- gets nothing to compare
    // against there, so it is checked again here: if the editor no longer
    // reads the way it did when the request started, something changed
    // out from under this load, and applying the fetched text would
    // silently discard it without ever asking.
    var textBeforeFetch = getRequestText();
    fetchTemplateText(key).then(function (text) {
      if (requestId !== templateRequestId) { return; }
      if (getRequestText() !== textBeforeFetch) {
        templateSelect.value = activeTemplateKey;
        say(
          "Not loading the " + TEMPLATE_LABELS[key] +
            " template: the request text changed while it was loading.",
          "error"
        );
        return;
      }
      applyTemplate(key, text);
      say(loadedMessage || ("Loaded the " + TEMPLATE_LABELS[key] + " template."), "ok");
    }).catch(function (error) {
      if (requestId !== templateRequestId) { return; }
      templateSelect.value = activeTemplateKey;
      say(String(error.message || error), "error");
    });
  }

  // Startup always loads Simple fresh over the network, never from a
  // cache, cookie, or local storage -- reloading the page is the only
  // reset this bench has, and it must actually reset (issue #140).
  loadTemplate("simple", "Loaded the Simple template. Choose an Audition source to begin.");

  templateSelect.addEventListener("change", function () {
    var key = templateSelect.value;
    var current = getRequestText();
    var unsaved = current !== lastLoadedTemplateText && current !== lastRenderedText;
    if (unsaved && !window.confirm(
      "Replace the current request text with the " + TEMPLATE_LABELS[key] +
        " template? Changes since the last load or render will be lost."
    )) {
      templateSelect.value = activeTemplateKey;
      return;
    }
    loadTemplate(key);
  });

  // Every message is assigned as text, never as markup: filenames,
  // requests, and renderer diagnostics are all untrusted content.
  function say(message, kind) {
    statusBox.textContent = message;
    statusBox.className = kind || "";
  }

  function describe(body) {
    var text = (body && body.reason) || "request failed";
    if (body && body.category) { text = body.category + ": " + text; }
    if (body && body.location) { text += "\nat " + body.location; }
    return text;
  }

  // One action at a time, so what is on screen is what the server holds.
  function setBusy(busy) {
    renderButton.disabled = busy;
    sourceInput.disabled = busy;
  }

  // Both endpoints answer JSON, and report failure the same way.
  function send(path, options) {
    return fetch(api(path), options).then(function (response) {
      return response.json().then(function (body) {
        if (!response.ok) { throw new Error(describe(body)); }
        return body;
      });
    });
  }

  // Triggers a real browser download of in-memory text -- on explicit
  // request only, and never through a plain <a href> the browser might
  // just navigate to instead of saving.
  function downloadText(filename, text, mimeType) {
    var blob = new Blob([text], { type: mimeType });
    var url = URL.createObjectURL(blob);
    var link = document.createElement("a");
    link.href = url;
    link.download = filename;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    setTimeout(function () { URL.revokeObjectURL(url); }, 0);
  }

  sourceInput.addEventListener("change", function () {
    var file = sourceInput.files && sourceInput.files[0];
    if (!file) { return; }
    // Selecting is asynchronous. Until the server has accepted the bytes,
    // Render would run against whatever source is still active, and a
    // second selection could land out of order and win. Hold both shut.
    setBusy(true);
    say("Reading " + file.name + "…");
    // The browser sends the selected bytes. The server is never given a
    // filesystem path -- the name travels only as a display label.
    file.arrayBuffer().then(function (bytes) {
      return send("/api/source", {
        method: "POST",
        headers: {
          "Content-Type": "application/octet-stream",
          "X-Source-Filename": file.name.replace(/[^\x20-\x7E]/g, "")
        },
        body: bytes
      });
    }).then(function (facts) {
      sourceName.textContent =
        facts.filename + " — " + facts.channels + " ch, " +
        facts.sampleRate + " Hz, " + facts.durationSeconds + " s";
      say("Source ready. Press Render.", "ok");
    }).catch(function (error) {
      // A refused selection never replaced the source on the server, so
      // the label must keep naming the one Render will actually use.
      var active = sourceName.textContent;
      say(
        String(error.message || error) +
          (active ? "\nStill using " + active : ""),
        "error"
      );
    }).then(function () {
      setBusy(false);
    });
  });

  renderButton.addEventListener("click", function () {
    setBusy(true);
    say("Rendering…");
    // Captured once, not re-read after the request settles: what counts
    // as "last successfully rendered" (issue #140's template dirty-check)
    // is the text actually sent, regardless of anything typed meanwhile.
    var sentText = getRequestText();
    send("/api/render", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: sentText
    }).then(function (facts) {
      lastRenderedText = sentText;
      factsBox.textContent =
        facts.sourceFilename + " — " + facts.durationSeconds + " s, " +
        facts.sampleRate + " Hz, " + facts.channels + " ch";
      // Cache-busted so a new render replaces the previous audio.
      var url = api("/api/output.wav") + "&n=" + Date.now();
      player.src = url;
      player.hidden = false;
      download.href = url;
      download.hidden = false;
      say("Rendered. Press play.", "ok");
    }).catch(function (error) {
      // A failed render leaves the previous playable result in place.
      say(String(error.message || error), "error");
    }).then(function () {
      setBusy(false);
    });
  });

  // A JSON pretty-printer that never routes numbers through a JavaScript
  // Number: the renderer accepts the full uint64 seed range
  // (tests/test_configuration_cli.py:343-378), which exceeds 2^53, and
  // JSON.parse/JSON.stringify would silently round such a seed to the
  // nearest double. Numeric and string lexemes are re-emitted verbatim;
  // only whitespace and structure change. Layout matches
  // JSON.stringify(_, null, 2): two-space indents, ": " after keys, no
  // trailing commas, "{}"/"[]" for empty containers.
  function formatJsonPreservingLexemes(text) {
    var i = 0;
    var n = text.length;

    function fail(message) {
      throw new SyntaxError(message + " at position " + i);
    }

    function skipWhitespace() {
      while (i < n && /[ \t\n\r]/.test(text[i])) { i++; }
    }

    function parseString() {
      var start = i;
      if (text[i] !== "\"") { fail("Expected string"); }
      i++;
      while (true) {
        if (i >= n) { fail("Unterminated string"); }
        var c = text[i];
        if (c === "\\") {
          i += 2;
        } else if (c === "\"") {
          i++;
          break;
        } else {
          i++;
        }
      }
      return text.slice(start, i);
    }

    function parseNumber() {
      var start = i;
      if (text[i] === "-") { i++; }
      if (!/[0-9]/.test(text[i] || "")) { fail("Invalid number"); }
      while (/[0-9]/.test(text[i] || "")) { i++; }
      if (text[i] === ".") {
        i++;
        if (!/[0-9]/.test(text[i] || "")) { fail("Invalid number"); }
        while (/[0-9]/.test(text[i] || "")) { i++; }
      }
      if (text[i] === "e" || text[i] === "E") {
        i++;
        if (text[i] === "+" || text[i] === "-") { i++; }
        if (!/[0-9]/.test(text[i] || "")) { fail("Invalid number"); }
        while (/[0-9]/.test(text[i] || "")) { i++; }
      }
      return { kind: "raw", raw: text.slice(start, i) };
    }

    function parseLiteral(word, kind) {
      if (text.slice(i, i + word.length) !== word) { fail("Invalid literal"); }
      i += word.length;
      return { kind: kind, raw: word };
    }

    function parseArray() {
      i++; // "["
      var items = [];
      skipWhitespace();
      if (text[i] === "]") { i++; return { kind: "array", items: items }; }
      while (true) {
        skipWhitespace();
        items.push(parseValue());
        skipWhitespace();
        if (text[i] === ",") { i++; continue; }
        if (text[i] === "]") { i++; break; }
        fail("Expected ',' or ']'");
      }
      return { kind: "array", items: items };
    }

    function parseObject() {
      i++; // "{"
      var members = [];
      skipWhitespace();
      if (text[i] === "}") { i++; return { kind: "object", members: members }; }
      while (true) {
        skipWhitespace();
        var key = parseString();
        skipWhitespace();
        if (text[i] !== ":") { fail("Expected ':'"); }
        i++;
        skipWhitespace();
        var value = parseValue();
        members.push({ key: key, value: value });
        skipWhitespace();
        if (text[i] === ",") { i++; continue; }
        if (text[i] === "}") { i++; break; }
        fail("Expected ',' or '}'");
      }
      return { kind: "object", members: members };
    }

    function parseValue() {
      skipWhitespace();
      var c = text[i];
      if (c === "{") { return parseObject(); }
      if (c === "[") { return parseArray(); }
      if (c === "\"") { return { kind: "raw", raw: parseString() }; }
      if (c === "t") { return parseLiteral("true", "raw"); }
      if (c === "f") { return parseLiteral("false", "raw"); }
      if (c === "n") { return parseLiteral("null", "raw"); }
      if (c === "-" || /[0-9]/.test(c || "")) { return parseNumber(); }
      fail("Unexpected token");
    }

    function serialize(node, depth) {
      var indent = "  ".repeat(depth);
      var childIndent = "  ".repeat(depth + 1);
      if (node.kind === "raw") { return node.raw; }
      if (node.kind === "array") {
        if (node.items.length === 0) { return "[]"; }
        var items = node.items.map(function (item) {
          return childIndent + serialize(item, depth + 1);
        });
        return "[\n" + items.join(",\n") + "\n" + indent + "]";
      }
      if (node.kind === "object") {
        if (node.members.length === 0) { return "{}"; }
        var members = node.members.map(function (member) {
          return childIndent + member.key + ": " + serialize(member.value, depth + 1);
        });
        return "{\n" + members.join(",\n") + "\n" + indent + "}";
      }
      fail("Unknown node");
    }

    var root = parseValue();
    skipWhitespace();
    if (i !== n) { fail("Unexpected trailing content"); }
    return serialize(root, 0);
  }

  // Explicit action, and the only one that ever rewrites the editor text.
  // Malformed JSON is reported and the text is left exactly as it was.
  formatButton.addEventListener("click", function () {
    var formatted;
    try {
      formatted = formatJsonPreservingLexemes(getRequestText());
    } catch (error) {
      say("Format JSON: " + String(error.message || error), "error");
      return;
    }
    setRequestText(formatted);
    say("Formatted.", "ok");
  });

  // The current editor string, regardless of validity or render status --
  // never the last-rendered or reformatted text -- on explicit request only.
  downloadRequestButton.addEventListener("click", function () {
    downloadText("request.json", getRequestText(), "application/json");
  });
})();
