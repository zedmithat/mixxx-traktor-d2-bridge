"use strict";

var fs = require("fs");
var vm = require("vm");
var values = {};
var parameters = {};
var sysex = [];
var calls = [];
var timers = [];

function k(group, key) { return group + "|" + key; }

global.engine = {
    getTrackLocation: function(group) {
        return group === "[Channel1]" ?
            "/media/pi/REKORDBOX/T\u00fcrk\u00e7e Par\u00e7a 01.mp3" :
            "/media/pi/REKORDBOX/Deck 2.mp3";
    },
    getValue: function(group, key) {
        if (values[k(group, key)] !== undefined) return values[k(group, key)];
        if (key === "duration") return 420;
        if (key === "bpm") return 128;
        if (key === "visual_bpm") return 129.57;
        if (key === "rate_ratio") return 1;
        if (key === "beat_prev") return 100;
        if (key === "beat_next") return 200;
        if (key === "track_samplerate") return 44100;
        if (key === "track_id") return group === "[Channel1]" ? 101 : 202;
        if (key.indexOf("hotcue_") === 0) return -1;
        if (key.indexOf("browse_track_id_") === 0) return 1;
        return 0;
    },
    setValue: function(group, key, value) {
        values[k(group, key)] = value;
        calls.push(["value", group, key, value]);
    },
    getParameter: function(group, key) {
        return parameters[k(group, key)] || 0;
    },
    setParameter: function(group, key, value) {
        parameters[k(group, key)] = value;
        calls.push(["parameter", group, key, value]);
    },
    makeConnection: function() { return {disconnect: function() {}}; },
    beginTimer: function(interval, callback, oneShot) {
        timers.push({interval: interval, callback: callback, oneShot: oneShot});
        return timers.length;
    },
    scratchEnable: function() {},
    scratchTick: function() {},
    scratchDisable: function() {},
    isScratching: function() { return false; },
    stopTimer: function() {}
};
global.midi = {
    sendSysexMsg: function(message, length) {
        if (message.length !== length || message[0] !== 0xF0 || message[length - 1] !== 0xF7)
            throw new Error("invalid SysEx");
        sysex.push(message);
    }
};

function decodeSysex(message) {
    return message.slice(2, message.length - 1).map(function(byte) {
        return String.fromCharCode(byte);
    }).join("");
}

function countPayloadPrefix(prefix) {
    return sysex.map(decodeSysex).filter(function(text) {
        return text.indexOf(prefix) === 0;
    }).length;
}

vm.runInThisContext(fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2-scripts.js", "utf8"));

/* Use meaningful, identical-on-retry live values. The one-shot startup retry
 * then exercises a warm JS cache after the bridge has reset its identity. */
values[k("[Channel1]", "track_loaded")] = 1;
values[k("[Channel2]", "track_loaded")] = 1;
values[k("[Channel1]", "playposition")] = 0.375;
values[k("[Channel2]", "playposition")] = 0.625;
values[k("[Channel1]", "play")] = 1;
values[k("[Channel2]", "play")] = 1;
values[k("[Channel1]", "rate_ratio")] = 1.05;
values[k("[Channel2]", "rate_ratio")] = 0.96;
values[k("[Channel1]", "visual_key")] = 20;
values[k("[Channel2]", "visual_key")] = 15;
values[k("[Channel1]", "beat_distance")] = 0.25;
values[k("[Channel2]", "beat_distance")] = 0.50;

D2.init("test", false);
var startupTrackCounts = {
    1: countPayloadPrefix("D2|1|TRACKID|"),
    2: countPayloadPrefix("D2|2|TRACKID|")
};
timers.filter(function(timer) { return timer.interval === 250; })[0].callback();
if (countPayloadPrefix("D2|1|TRACKID|") !== startupTrackCounts[1] ||
    countPayloadPrefix("D2|2|TRACKID|") !== startupTrackCounts[2])
    throw new Error("startup fallback duplicated an already published identity");

/* A 500 ms resilience tick can win the race against trackLoaded's 120 ms
 * settle callback. Only the winner may submit TRACKID/LOAD. */
D2.trackLoaded("[Channel1]", 1);
D2.updateDisplays();
var raceTrackCount = countPayloadPrefix("D2|1|TRACKID|");
timers.filter(function(timer) { return timer.interval === 120; }).pop().callback();
if (countPayloadPrefix("D2|1|TRACKID|") !== raceTrackCount)
    throw new Error("track-loaded settle timer duplicated a completed identity");

/* Legitimate reload of unchanged transport/key values must still replay all
 * bridge-reset fields despite a warm sendState cache. */
D2.publishTrackLoad("[Channel1]");
D2.publishTrackLoad("[Channel2]");
var groups = ["[Channel1]", "[Channel2]"];
groups.forEach(function(group) {
    D2.browseTouch(0, 0x64, 0x7F, 0, group);
    if (!calls.some(function(call) {
        return call[0] === "value" && call[1] === "[Tab]" &&
            call[2] === "current" && call[3] === D2.skinPage.browse;
    })) throw new Error(group + " browse touch did not open the zed Browse page");
    var beforeBrowse = calls.length;
    D2.browseEncoder(0, 0x14, 0x41, 0, group);
    if (!calls.slice(beforeBrowse).some(function(call) {
        return call[0] === "value" && call[2] === "d2_track_down" && call[3] === 1;
    })) throw new Error("clockwise browse did not pulse direct D2 track down");
    beforeBrowse = calls.length;
    D2.shiftButton(0, 0x5A, 0x7F, 0, group);
    D2.browseEncoder(0, 0x14, 0x3F, 0, group);
    if (!calls.slice(beforeBrowse).some(function(call) {
        return call[0] === "value" && call[2] === "d2_track_up" && call[3] === 1;
    })) throw new Error("counter-clockwise browse did not pulse direct D2 track up");
    D2.shiftButton(0, 0x5A, 0, 0, group);
    D2.backButton(0, 0x3F, 0x7F, 0, group);
    var beforeTreePress = calls.length;
    D2.sidebarActivate(0, 0x3D, 0x7F, 0, group);
    var treeCalls = calls.slice(beforeTreePress);
    if (!treeCalls.some(function(call) {
        return call[0] === "value" && call[1] === "[Library]" &&
            call[2] === "d2_sidebar_activate" && call[3] === 1;
    })) throw new Error("sidebar press did not activate direct D2 sidebar control");
    if (treeCalls.some(function(call) { return call[2] === "LoadSelectedTrack"; }))
        throw new Error("sidebar press unexpectedly loaded a track");
    D2.libraryFocus = 0;
    values[k("[Library]", "focused_widget")] = 3;
    if (typeof D2.loadSelectedTrack !== "undefined")
        throw new Error("track-list press still has a duplicate JS load path");
    var sortStart = calls.length;
    D2.leftScreenButton(0, 0x31, 0x7F, 0, group);
    D2.leftScreenButton(0, 0x32, 0x7F, 0, group);
    D2.leftScreenButton(0, 0x33, 0x7F, 0, group);
    D2.leftScreenButton(0, 0x34, 0x7F, 0, group);
    var sortCalls = calls.slice(sortStart);
    [2, 15, 20].forEach(function(column) {
        if (!sortCalls.some(function(call) {
            return call[0] === "value" && call[1] === "[Library]" &&
                call[2] === "sort_column_toggle" && call[3] === column;
        })) throw new Error("Browse sort column was not dispatched: " + column);
    });
    if (!sortCalls.some(function(call) {
        return call[0] === "value" && call[1] === "[Library]" &&
            call[2] === "sort_order";
    })) throw new Error("Browse sort direction was not toggled");
    values[k("[Tab]", "current")] = D2.skinPage.player;
    var zoomBefore = D2.zoomLevel[group];
    D2.leftScreenButton(0, 0x31, 0x7F, 0, group);
    if (D2.zoomLevel[group] === zoomBefore)
        throw new Error("player-context left screen controls were replaced by Browse sort");
    values[k("[Tab]", "current")] = D2.skinPage.browse;
    for (var note = 0x35; note <= 0x38; note++)
        D2.rightScreenButton(0, note, 0x7F, 0, group);
    ["HOTCUE", "LOOP", "FREEZE", "SAMPLER", "BEATJUMP"].forEach(function(mode) {
        D2.setPerformanceMode(group, mode);
        for (var pad = 0x4C; pad <= 0x53; pad++) D2.padButton(0, pad, 0x7F, 0, group);
    });
    for (var cc = 0x24; cc <= 0x27; cc++) D2.fxKnob(0, cc, 64, 0, group);
    for (note = 0x28; note <= 0x2B; note++) D2.fxButton(0, note, 0x7F, 0, group);
    for (note = 0x2C; note <= 0x2F; note++) {
        D2.fxTouch(0, note, 0x7F, 0, group);
        D2.fxTouch(0, note, 0, 0, group);
    }
    D2.touchStripTouch(0, 0x5E, 0x7F, 0, group);
    D2.touchStrip(0, 0x28, 64, 0, group);
    D2.touchStripTouch(0, 0x5E, 0, 0, group);
    D2.captureButton(0, 0x40, 0x7F, 0, group);
    var usbStart = calls.length;
    D2.usbOpenButton(0, 0x65, 0x7F, 0, group);
    if (!calls.slice(usbStart).some(function(call) {
        return call[0] === "value" && call[1] === "[Library]" &&
            call[2] === "d2_usb_open" && call[3] === 1;
    })) throw new Error("Browse CAPTURE did not pulse native USB open");
    D2.editButton(0, 0x41, 0x7F, 0, group);
    D2.screenEncoder(0, 0x10, 0x41, 0, group);
    D2.screenEncoder(0, 0x11, 0x3F, 0, group);
});
D2.shutdown();

if (sysex.length < 20 || calls.length < 100) throw new Error("coverage failure");
var sysexText = sysex.map(decodeSysex);
["CHANNEL", "SAMPLERATE"].forEach(function(unconsumedKey) {
    if (sysexText.some(function(text) {
        return text.indexOf("|" + unconsumedKey + "|") >= 0;
    })) throw new Error("unconsumed " + unconsumedKey + " payload was emitted");
});
function payloadIndex(payload) {
    return sysexText.indexOf(payload);
}
var track1 = payloadIndex("D2|1|TRACKID|101");
var load1 = sysexText.findIndex(function(text) {
    return text.indexOf("D2|1|LOAD|") === 0;
});
var track2 = payloadIndex("D2|2|TRACKID|202");
var load2 = sysexText.findIndex(function(text) {
    return text.indexOf("D2|2|LOAD|") === 0;
});
var bpmAfterLoad1 = sysexText.findIndex(function(text, index) {
    return index > load1 && text === "D2|1|BPM|129.57";
});
var bpmAfterLoad2 = sysexText.findIndex(function(text, index) {
    return index > load2 && text === "D2|2|BPM|129.57";
});
if (track1 < 0 || load1 < 0 || track1 > load1 || bpmAfterLoad1 < 0 ||
    track2 < 0 || load2 < 0 || track2 > load2 || bpmAfterLoad2 < 0)
    throw new Error("track identity transaction must publish TRACKID < LOAD < live BPM");

/* TRACKID resets every track-derived bridge field, not only BPM. The startup
 * retry deliberately repeats the same identity after the JS cache is warm,
 * so every meaningful field below must still be forced after the last LOAD. */
var lastLoad = {
    1: sysexText.map(function(text, index) {
        return text.indexOf("D2|1|LOAD|") === 0 ? index : -1;
    }).reduce(function(a, b) { return Math.max(a, b); }, -1),
    2: sysexText.map(function(text, index) {
        return text.indexOf("D2|2|LOAD|") === 0 ? index : -1;
    }).reduce(function(a, b) { return Math.max(a, b); }, -1)
};
var postIdentityFields = {
    1: ["DURATION|420.000", "RATE|1.05000", "POS|0.375000", "PLAY|1",
        "REMAIN|262.500", "BEATDIST|0.250000", "BEATVALID|1",
        "BEATPREV|0.00000540", "BEATNEXT|0.00001080", "KEYVISUAL|20",
        "PHASE|0.00000,0.00000,0,0,1,1,2"],
    2: ["DURATION|420.000", "RATE|0.96000", "POS|0.625000", "PLAY|1",
        "REMAIN|157.500", "BEATDIST|0.500000", "BEATVALID|1",
        "BEATPREV|0.00000540", "BEATNEXT|0.00001080", "KEYVISUAL|15",
        "PHASE|0.00000,0.00000,0,0,1,1,2"]
};
[1, 2].forEach(function(deck) {
    postIdentityFields[deck].forEach(function(field) {
        var payload = "D2|" + deck + "|" + field;
        if (!sysexText.some(function(text, index) {
            return index > lastLoad[deck] && text === payload;
        })) throw new Error("post-identity live snapshot missing " + payload);
    });
});
values[k("[Channel1]", "track_id")] = 0;
D2.pendingTrackId["[Channel1]"] = 0;
var fallbackStart = sysex.length;
D2.publishTrackLoad("[Channel1]");
var fallbackPayloads = sysex.slice(fallbackStart).map(decodeSysex);
var locBegin = fallbackPayloads.findIndex(function(text) {
    return text.indexOf("D2|1|LOCBEGIN|") === 0;
});
var locChunk = fallbackPayloads.findIndex(function(text) {
    return text.indexOf("D2|1|LOCCHUNK|") === 0;
});
var locEnd = fallbackPayloads.indexOf("D2|1|LOCEND|1");
var fallbackLoad = fallbackPayloads.findIndex(function(text) {
    return text.indexOf("D2|1|LOAD|") === 0;
});
if (locBegin < 0 || locChunk <= locBegin || locEnd <= locChunk ||
    fallbackLoad <= locEnd)
    throw new Error("exact track-location fallback was not sent before LOAD");
if (!sysexText.some(function(text) { return text.indexOf("|LOOPSIZE|4.00000") >= 0; }))
    throw new Error("loop size was not published to the D2 renderer");
values[k("[Channel1]", "visual_bpm")] = 127.386;
var liveBpmStart = sysex.length;
D2.liveStateChanged("[Channel1]", "visual_bpm", 127.386);
var liveBpmPayloads = sysex.slice(liveBpmStart).map(decodeSysex);
if (liveBpmPayloads.indexOf("D2|1|BPM|127.39") < 0)
    throw new Error("live visual_bpm did not retain two-decimal precision");
if (!sysexText.some(function(text) { return text.indexOf("|BROWSE8|") >= 0; }))
    throw new Error("ninth Browse row was not published");
var bridgeSource = fs.readFileSync(__dirname + "/../bridge/d2_bridge.c", "utf8");
if (bridgeSource.indexOf("WHERE l.id = ?") < 0 ||
    bridgeSource.indexOf("WHERE tl.location = ?") < 0 ||
    bridgeSource.indexOf("ORDER BY ABS(l.duration - ?)") >= 0)
    throw new Error("bridge metadata lookup is not exact track-ID based");
if (!/browse_focus\s*\?\s*61\s*:\s*62/.test(bridgeSource))
    throw new Error("bridge does not physically multiplex sidebar Note 61 and track Note 62");
if (!/browse_press_note\[player\]\s*=\s*\(uint8_t\)browse_note/.test(bridgeSource) ||
    !/browse_note\s*=\s*browse_press_note\[player\]\s*\?/.test(bridgeSource) ||
    !/browse_press_note\[player\]\s*=\s*0/.test(bridgeSource))
    throw new Error("Browse press/release does not latch the selected Note 61/62");
if (/BROWSE 30MS FILTER|browse_last_us|browse_last_delta/.test(bridgeSource))
    throw new Error("bridge still drops legitimate rapid Browse detents");
if (!/d2_browse_delta_steps\(delta\)/.test(bridgeSource) ||
    !/step\s*<\s*step_count/.test(bridgeSource) ||
    !/D2_BROWSE_MAX_STEPS_PER_REPORT/.test(bridgeSource))
    throw new Error("Browse delta magnitude is not emitted as bounded MIDI steps");
var browseTouchEmitters = bridgeSource.match(
    /midi_note\s*\(\s*midi_channel\s*,\s*100\s*,/g) || [];
if (browseTouchEmitters.length !== 1)
    throw new Error("bridge must have exactly one physical Browse-touch Note 100 emitter");
console.log("D2_MAPPING_TEST_OK sysex=" + sysex.length + " calls=" + calls.length);
