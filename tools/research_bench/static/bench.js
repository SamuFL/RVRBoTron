(function () {
  "use strict";

  var token = new URLSearchParams(window.location.search).get("token") || "";
  var sourceInput = document.getElementById("source");
  var sourceName = document.getElementById("source-name");
  var requestBox = document.getElementById("request");
  var renderButton = document.getElementById("render");
  var statusBox = document.getElementById("status");
  var factsBox = document.getElementById("facts");
  var player = document.getElementById("player");
  var download = document.getElementById("download");

  // A starting request, so the bench is usable before reading a guide.
  // Edit it freely: this text, not this file, is what gets rendered.
  requestBox.value = JSON.stringify({
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
  }, null, 2);

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
    // The textarea's current string is the request, sent through as-is.
    send("/api/render", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: requestBox.value
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
})();
