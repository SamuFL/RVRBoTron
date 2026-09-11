(function () {
  "use strict";

  var token = new URLSearchParams(window.location.search).get("token") || "";
  var sourceInput = document.getElementById("source");
  var sourceName = document.getElementById("source-name");
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

  // A starting request, so the bench is usable before reading a guide.
  // Edit it freely: this text, not this file, is what gets rendered.
  setRequestText(JSON.stringify({
    formatVersion: 2,
    seed: 42,
    composition: {
      stages: [
        { type: "split", channels: 8 },
        { type: "diffuser", steps: 4, totalMs: 80 },
        { type: "feedback-loop", delayMinMs: 60, delayMaxMs: 120, rt60Sec: 2.0 },
        { type: "downmix", strategy: "orthogonal-rows" }
      ],
      preDelayMs: 20,
      dryDb: 0,
      wetDb: -3,
      wetOnly: false
    }
  }, null, 2));

  function api(path) {
    return path + "?token=" + encodeURIComponent(token);
  }

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
    // The editor's current string is sent through as-is.
    send("/api/render", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: getRequestText()
    }).then(function (facts) {
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

  // Explicit action, and the only one that ever rewrites the editor text.
  // Malformed JSON is reported and the text is left exactly as it was.
  formatButton.addEventListener("click", function () {
    var parsed;
    try {
      parsed = JSON.parse(getRequestText());
    } catch (error) {
      say("Format JSON: " + String(error.message || error), "error");
      return;
    }
    setRequestText(JSON.stringify(parsed, null, 2));
    say("Formatted.", "ok");
  });

  // The current editor string, regardless of validity or render status --
  // never the last-rendered or reformatted text -- on explicit request only.
  downloadRequestButton.addEventListener("click", function () {
    downloadText("request.json", getRequestText(), "application/json");
  });
})();
