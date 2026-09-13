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

  function getRequestText() {
    return editor.getValue();
  }

  function setRequestText(text) {
    editor.setValue(text, -1); // -1: cursor at the start, nothing selected
  }

  function api(path) {
    return path + "?token=" + encodeURIComponent(token);
  }

  var TEMPLATE_LABELS = {
    simple: "Simple",
    full: "Full",
    modulated: "Modulated",
    spatial: "Spatial"
  };

  var lastLoadedTemplateText = null;
  var lastRenderedText = null;
  var activeTemplateKey = "simple";

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
    setBusy(true);
    say("Reading " + file.name + "…");
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

  downloadRequestButton.addEventListener("click", function () {
    downloadText("request.json", getRequestText(), "application/json");
  });
})();
