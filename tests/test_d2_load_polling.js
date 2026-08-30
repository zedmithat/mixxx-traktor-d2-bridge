"use strict";

/*
 * Deterministic ownership contract for the native bridge's LOAD path.
 *
 * A wall-clock test with a slow fake ffmpeg/sqlite helper would be flaky and
 * would still need a production injection seam.  Instead, this test verifies
 * the invariant that guarantees HID responsiveness: d2_parse_sysex() may only
 * publish a bounded load request. Every filesystem/database/helper operation
 * belongs to a separately-created worker and must run outside d2_state_mutex.
 */

var fs = require("fs");
var path = require("path");
var source = fs.readFileSync(
    path.join(__dirname, "..", "bridge", "d2_bridge.c"), "utf8");

function fail(message) { throw new Error(message); }
function assert(condition, message) { if (!condition) fail(message); }

/* Replace comments and string/character literals with spaces while retaining
 * byte offsets. This makes brace matching and call-graph checks insensitive
 * to documentation text or braces embedded in format strings. */
function maskNonCode(text) {
    var output = text.split("");
    var state = "code";
    var escaped = false;
    for (var i = 0; i < text.length; ++i) {
        var ch = text[i];
        var next = i + 1 < text.length ? text[i + 1] : "";
        if (state === "code") {
            if (ch === "/" && next === "/") {
                output[i] = output[i + 1] = " ";
                ++i;
                state = "line";
            } else if (ch === "/" && next === "*") {
                output[i] = output[i + 1] = " ";
                ++i;
                state = "block";
            } else if (ch === "\"") {
                output[i] = " ";
                state = "string";
                escaped = false;
            } else if (ch === "'") {
                output[i] = " ";
                state = "char";
                escaped = false;
            }
        } else if (state === "line") {
            if (ch === "\n") state = "code";
            else output[i] = " ";
        } else if (state === "block") {
            if (ch === "*" && next === "/") {
                output[i] = output[i + 1] = " ";
                ++i;
                state = "code";
            } else if (ch !== "\n") output[i] = " ";
        } else {
            if (ch !== "\n") output[i] = " ";
            if (escaped) {
                escaped = false;
            } else if (ch === "\\") {
                escaped = true;
            } else if ((state === "string" && ch === "\"") ||
                       (state === "char" && ch === "'")) {
                state = "code";
            }
        }
    }
    return output.join("");
}

var maskedSource = maskNonCode(source);

function balancedEnd(masked, openBrace) {
    var depth = 0;
    for (var i = openBrace; i < masked.length; ++i) {
        if (masked[i] === "{") ++depth;
        else if (masked[i] === "}" && --depth === 0) return i + 1;
    }
    fail("unbalanced C function starting at byte " + openBrace);
}

function extractFunction(name) {
    var pattern = new RegExp("\\b" + name +
        "\\s*\\([^;{}]*\\)\\s*\\{", "m");
    var match = pattern.exec(maskedSource);
    if (!match) return null;
    var open = maskedSource.indexOf("{", match.index);
    var end = balancedEnd(maskedSource, open);
    return {
        name: name,
        start: match.index,
        end: end,
        source: source.slice(match.index, end),
        masked: maskedSource.slice(match.index, end)
    };
}

/* Index every project-local function so a synchronous wrapper cannot hide a
 * slow call one level below d2_parse_sysex(). */
var functions = {};
var definitionPattern = /\b(d2_[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*\{/gm;
var definition;
while ((definition = definitionPattern.exec(maskedSource)) !== null) {
    if (!functions[definition[1]])
        functions[definition[1]] = extractFunction(definition[1]);
}

function localCalls(fn) {
    var calls = [];
    var pattern = /\b(d2_[A-Za-z0-9_]+)\s*\(/g;
    var match;
    while ((match = pattern.exec(fn.masked)) !== null) {
        if (match[1] !== fn.name)
            calls.push({name: match[1], offset: match.index});
    }
    return calls;
}

var slowLoadFunctions = {
    d2_resolve_track_id_by_location: true,
    d2_load_track_metadata: true,
    d2_load_real_waveform: true,
    d2_load_mixxx_waveform: true,
    d2_load_cover_art: true,
    d2_load_mixxx_beatgrid: true,
    d2_build_wave_strip: true
};

function slowPathFrom(name, visiting) {
    if (slowLoadFunctions[name]) return [name];
    if (!functions[name]) return null;
    visiting = visiting || {};
    if (visiting[name]) return null;
    visiting[name] = true;
    var calls = localCalls(functions[name]);
    for (var i = 0; i < calls.length; ++i) {
        var child = slowPathFrom(calls[i].name, visiting);
        if (child) {
            delete visiting[name];
            return [name].concat(child);
        }
    }
    delete visiting[name];
    return null;
}

var parser = functions.d2_parse_sysex;
assert(parser, "d2_parse_sysex() source was not found");
var parserSlowPath = slowPathFrom("d2_parse_sysex");
assert(!parserSlowPath,
    "HID polling can block: synchronous SysEx path reaches " +
    (parserSlowPath ? parserSlowPath.join(" -> ") : "none"));

/* LOAD must still do real work: it should publish to a named asynchronous
 * queue/request function rather than becoming an accidental no-op. */
var loadMarker = parser.source.indexOf('strcmp(key, "LOAD")');
assert(loadMarker >= 0, "LOAD SysEx branch was not found");
var absoluteLoadMarker = parser.start + loadMarker;
var loadOpen = maskedSource.indexOf("{", absoluteLoadMarker);
var loadEnd = balancedEnd(maskedSource, loadOpen);
var loadBranchMask = maskedSource.slice(loadOpen, loadEnd);
var loadCalls = [];
var loadCallPattern = /\b(d2_[A-Za-z0-9_]+)\s*\(/g;
var loadCall;
while ((loadCall = loadCallPattern.exec(loadBranchMask)) !== null)
    loadCalls.push(loadCall[1]);
var asyncLoadName = /(?:(?:queue|enqueue|submit|schedule|request).*(?:load|track|metadata)|(?:load|track|metadata).*(?:queue|enqueue|submit|schedule|request))/i;
var queueCall = loadCalls.filter(function(name) {
    return asyncLoadName.test(name);
})[0];
assert(queueCall,
    "LOAD branch does not publish to a dedicated asynchronous load queue");
assert(functions[queueCall], queueCall + "() definition was not found");
assert(/pthread_cond_(?:signal|broadcast)\s*\(|sem_post\s*\(|eventfd_write\s*\(/
        .test(functions[queueCall].masked),
    queueCall + "() does not wake a worker without polling/blocking HID");

/* Find pthread workers from actual pthread_create ownership, not by assuming
 * a particular variable/function name. The render worker does not own LOAD;
 * at least one other worker must reach the slow metadata/waveform path. */
var workerNames = [];
var workerPattern = /pthread_create\s*\([^,]+,[^,]+,\s*(d2_[A-Za-z0-9_]+)\s*,/g;
var workerMatch;
while ((workerMatch = workerPattern.exec(maskedSource)) !== null)
    workerNames.push(workerMatch[1]);
var loadWorker = null;
for (var workerIndex = 0; workerIndex < workerNames.length; ++workerIndex) {
    if (workerNames[workerIndex] !== "d2_render_thread_main" &&
        slowPathFrom(workerNames[workerIndex])) {
        loadWorker = workerNames[workerIndex];
        break;
    }
}
assert(loadWorker,
    "no separately-created pthread owns the slow LOAD helper path");

/* A worker that keeps d2_state_mutex while waiting on sqlite/ffmpeg would
 * merely move the same input stall to another thread. Walk every reachable
 * local function and reject slow calls made while that mutex is held. */
function verifyUnlockedSlowCalls(name, visited) {
    if (!functions[name] || visited[name]) return;
    visited[name] = true;
    var fn = functions[name];
    var events = [];
    var eventPattern = /pthread_mutex_(lock|unlock)\s*\(\s*&d2_state_mutex\s*\)|\b(d2_[A-Za-z0-9_]+)\s*\(/g;
    var event;
    while ((event = eventPattern.exec(fn.masked)) !== null) {
        events.push({offset: event.index, kind: event[1] || "call",
                     target: event[2] || ""});
    }
    events.sort(function(left, right) { return left.offset - right.offset; });
    var locked = 0;
    for (var i = 0; i < events.length; ++i) {
        if (events[i].kind === "lock") ++locked;
        else if (events[i].kind === "unlock") locked = Math.max(0, locked - 1);
        else if (events[i].target !== name) {
            var slowPath = slowPathFrom(events[i].target);
            assert(!(locked && slowPath),
                name + "() holds d2_state_mutex across " +
                (slowPath ? slowPath.join(" -> ") : "none"));
            verifyUnlockedSlowCalls(events[i].target, visited);
        }
    }
}
verifyUnlockedSlowCalls(loadWorker, {});

var mainFunction = extractFunction("main");
assert(mainFunction && /\bctlra_idle_iter\s*\(/.test(mainFunction.masked),
    "live main loop no longer owns libctlra HID polling");
assert(/\bmidi_input_poll\s*\(/.test(mainFunction.masked),
    "live main loop no longer drains controller SysEx state");

console.log("D2_LOAD_POLLING_CONTRACT_OK worker=" + loadWorker +
            " queue=" + queueCall);
