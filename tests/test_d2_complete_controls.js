"use strict";

var fs = require("fs");
var vm = require("vm");
var values = {};
var parameters = {};
var calls = [];
var sysex = [];
var connections = [];
var timers = [];

function key(group, control) { return group + "|" + control; }
function assert(condition, message) { if (!condition) throw new Error(message); }
function called(group, control, value) {
    return calls.some(function(call) {
        return call[1] === group && call[2] === control &&
            (value === undefined || call[3] === value);
    });
}
function resetCalls() { calls.length = 0; }

global.engine = {
    getValue: function(group, control) {
        if (values[key(group, control)] !== undefined) return values[key(group, control)];
        if (control === "duration") return 420;
        if (control === "bpm") return 128;
        if (control === "rate_ratio") return 1;
        if (control === "beat_prev") return 100;
        if (control === "beat_next") return 200;
        if (control === "track_samplerate") return 44100;
        if (control === "beatloop_size") return 4;
        if (control.indexOf("hotcue_") === 0 && control.indexOf("_position") > 0) return -1;
        if (control.indexOf("browse_track_id_") === 0) return 1;
        return 0;
    },
    setValue: function(group, control, value) {
        values[key(group, control)] = value;
        calls.push(["value", group, control, value]);
    },
    getParameter: function(group, control) { return parameters[key(group, control)] || 0; },
    setParameter: function(group, control, value) {
        parameters[key(group, control)] = value;
        calls.push(["parameter", group, control, value]);
    },
    makeConnection: function(group, control, callback) {
        var connection = {group: group, control: control, callback: callback,
                          disconnected: false};
        connections.push(connection);
        return {disconnect: function() { connection.disconnected = true; }};
    },
    beginTimer: function(interval, callback) {
        timers.push({interval: interval, callback: callback});
        return timers.length;
    },
    stopTimer: function() {}
};
global.midi = {
    sendSysexMsg: function(message, length) {
        assert(message.length === length, "invalid SysEx length");
        sysex.push(message);
    }
};

vm.runInThisContext(fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2-scripts.js", "utf8"));
values[key("[Channel1]", "beat_distance")] = 0.25;
values[key("[Channel2]", "beat_distance")] = 0.50;
values[key("[Channel1]", "sync_leader")] = 1;
values[key("[Channel1]", "visual_key")] = 20;
values[key("[Channel2]", "visual_key")] = 15;
D2.init("complete-test", false);

function sysexText(message) {
    return message.slice(2, message.length - 1).map(function(value) {
        return String.fromCharCode(value);
    }).join("");
}
assert(timers.some(function(timer) { return timer.interval === 500; }),
    "500 ms cached phase recovery timer missing");
assert(sysex.some(function(message) {
    return sysexText(message).indexOf("D2|1|PHASE|0.00000,0.00000,0,0") === 0;
}), "deck 1 phase payload missing");
assert(sysex.some(function(message) {
    return sysexText(message).indexOf("D2|2|PHASE|0.00000,0.00000,0,0") === 0;
}), "deck 2 phase payload missing");
assert(sysex.some(function(message) {
    return sysexText(message).indexOf("D2|1|KEYVISUAL|20") === 0;
}), "live visual key payload missing");
assert(connections.filter(function(connection) {
    return connection.control.indexOf("hotcue_") === 0 &&
           connection.control.indexOf("_position") > 0;
}).length === 16, "live Hotcue position connections missing");
assert(connections.filter(function(connection) {
    return connection.control === "beat_active";
}).length === 2, "per-beat phase-step connections missing");
var beatConnection = connections.filter(function(connection) {
    return connection.group === "[Channel1]" &&
           connection.control === "beat_active";
})[0];
beatConnection.callback(1);
beatConnection.callback(1);
assert(D2.phaseBeatStep["[Channel1]"] === 1,
    "phase cell advanced more than once during one beat pulse");
beatConnection.callback(0);
beatConnection.callback(1);
assert(D2.phaseBeatStep["[Channel1]"] === 2,
    "phase cell did not advance on the next beat");
var hotcueConnection = connections.filter(function(connection) {
    return connection.group === "[Channel1]" &&
           connection.control === "hotcue_1_position";
})[0];
var sysexBeforeHotcue = sysex.length;
values[key("[Channel1]", "hotcue_1_position")] = 44100;
hotcueConnection.callback(44100);
assert(sysex.length > sysexBeforeHotcue && sysex.some(function(message) {
    return sysexText(message).indexOf("D2|1|CUE1|") === 0;
}), "Hotcue overview did not refresh from a main-UI change");

var surfaces = ["[Channel1]", "[Channel2]"];
surfaces.forEach(function(surface, surfaceIndex) {
    var initialDeck = surfaceIndex + 1;
    var initialGroup = "[Channel" + initialDeck + "]";

    resetCalls();
    values[key(initialGroup, "play")] = 0;
    values[key(initialGroup, "playposition")] = 0.25;
    values[key(initialGroup, "cue_point")] = 22050;
    values[key(initialGroup, "track_samples")] = 88200;
    D2.playButton(0, 0x5D, 0x7F, 0, surface);
    assert(called(initialGroup, "play"), surface + " play not routed");
    resetCalls();
    values[key(initialGroup, "play")] = 1;
    D2.cueButton(0, 0x5C, 0x7F, 0, surface);
    assert(called(initialGroup, "cue_gotoandstop", 1),
        surface + " playing cue did not return/stop");
    resetCalls();
    values[key(initialGroup, "play")] = 0;
    values[key(initialGroup, "playposition")] = 0.25;
    D2.cueButton(0, 0x5C, 0x7F, 0, surface);
    assert(called(initialGroup, "cue_preview", 1),
        surface + " cue preview did not start at cue");
    values[key(initialGroup, "play")] = 1;
    D2.playButton(0, 0x5D, 0x7F, 0, surface);
    assert(called(initialGroup, "play", 0),
        surface + " cue-play latch did not use Mixxx native latch path");
    D2.cueButton(0, 0x5C, 0, 0, surface);
    assert(!called(initialGroup, "cue_preview", 0),
        surface + " cue release stopped latched playback");

    resetCalls();
    values[key(initialGroup, "play")] = 0;
    values[key(initialGroup, "playposition")] = 0.60;
    D2.cueButton(0, 0x5C, 0x7F, 0, surface);
    assert(called(initialGroup, "cue_set", 1),
        surface + " paused cue did not set a new cue point");
    assert(called(initialGroup, "cue_preview", 1),
        surface + " new cue did not start preview");
    D2.cueButton(0, 0x5C, 0, 0, surface);
    assert(called(initialGroup, "cue_preview", 0),
        surface + " cue preview release did not return to cue");

    resetCalls();
    D2.shiftButton(0, 0x5A, 0x7F, 0, surface);
    D2.cueButton(0, 0x5C, 0x7F, 0, surface);
    assert(called(initialGroup, "cue_gotoandstop", 1), surface + " shift cue missing");
    D2.setPerformanceMode(surface, "HOTCUE");
    D2.padButton(0, 0x4C, 0x7F, 0, surface);
    assert(called(initialGroup, "hotcue_1_clear", 1), surface + " shift hotcue clear missing");
    D2.shiftButton(0, 0x5A, 0, 0, surface);

    resetCalls();
    D2.fluxButton(0, 0x58, 0x7F, 0, surface);
    assert(called(initialGroup, "slip_enabled"), surface + " flux is not slip");

    resetCalls();
    D2.loopEncoder(0, 0x15, 0x41, 0, surface);
    D2.loopEncoder(0, 0x15, 0x3F, 0, surface);
    D2.loopPress(0, 0x42, 0x7F, 0, surface);
    D2.loopTouch(0, 0x43, 0x7F, 0, surface);
    D2.loopTouch(0, 0x43, 0, 0, surface);
    assert(called(initialGroup, "loop_double", 1), surface + " loop double missing");
    assert(called(initialGroup, "loop_halve", 1), surface + " loop halve missing");
    assert(called(initialGroup, "beatloop_activate", 1), surface + " loop press missing");

    resetCalls();
    for (var encoder = 0; encoder < 4; encoder++) {
        D2.screenEncoder(0, 0x10 + encoder, 0x41, 0, surface);
        D2.screenEncoderTouch(0, 0x39 + encoder, 0x7F, 0, surface);
        D2.screenEncoderTouch(0, 0x39 + encoder, 0, 0, surface);
        assert(called("[Sampler" + (encoder + 1) + "]", "pregain_up_small", 1),
            surface + " sampler knob " + (encoder + 1) + " missing");
    }

    resetCalls();
    for (var strip = 0; strip < 4; strip++) {
        D2.stemFader(0, 0x20 + strip, 96, 0, surface);
        D2.stemMute(0, 0x44 + strip, 0x7F, 0, surface);
        D2.faderTouch(0, 0x48 + strip, 0x7F, 0, surface);
        D2.faderTouch(0, 0x48 + strip, 0, 0, surface);
        assert(called("[Sampler" + (strip + 1) + "]", "volume"),
            surface + " sampler fader " + (strip + 1) + " missing");
        assert(called("[Sampler" + (strip + 1) + "]", "mute"),
            surface + " sampler mute " + (strip + 1) + " missing");
    }

    resetCalls();
    for (var deck = 0; deck < 4; deck++)
        D2.fxAssignButton(0, 0x24 + deck, 0x7F, 0, surface);
    assert(called(D2.effectUnitGroup(surface), "group_[Channel1]_enable"),
        surface + " FX deck assignment missing");
    var oldUnit = D2.fxUnit[surface];
    D2.fxSelectButton(0, 0x30, 0x7F, 0, surface);
    assert(D2.fxUnit[surface] !== oldUnit, surface + " FX unit did not change");

    resetCalls();
    D2.deckButton(0, 0x59, 0x7F, 0, surface);
    assert(D2.activeDeck[surface] === initialDeck, surface + " deck button changed two-deck routing");
});

var xml = fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2.midi.xml", "utf8");
[
    "D2.fxAssignButton", "D2.fxSelectButton", "D2.playButton", "D2.cueButton",
    "D2.loopEncoder", "D2.loopPress", "D2.loopTouch", "D2.screenEncoderTouch",
    "D2.faderTouch", "D2.stemFader", "D2.stemMute", "D2.touchStrip"
].forEach(function(binding) {
    assert(xml.indexOf("<key>" + binding + "</key>") >= 0, "XML missing " + binding);
});
[
    "0x12", "0x13", "0x15", "0x24", "0x27", "0x30", "0x39", "0x3C",
    "0x42", "0x43", "0x48", "0x4B", "0x70", "0x71"
].forEach(function(midiNumber) {
    assert(xml.indexOf("<midino>" + midiNumber + "</midino>") >= 0,
        "XML missing MIDI " + midiNumber);
});

/* libctlra emits ordinary D2 buttons as note 36 + hardware ID, except CUE
 * and PLAY which deliberately use dedicated CCs for reliable release events.
 * Browse press/touch use the bridge's 62/100 messages. */
function hasMidi(status, midiNumber) {
    var statusText = "<status>0x" + status.toString(16).toUpperCase() + "</status>";
    var midiText = "<midino>0x" + midiNumber.toString(16).toUpperCase() + "</midino>";
    var offset = 0;
    while ((offset = xml.indexOf(statusText, offset)) >= 0) {
        var end = xml.indexOf("</control>", offset);
        if (end >= 0 && xml.slice(offset, end).indexOf(midiText) >= 0) return true;
        offset += statusText.length;
    }
    return false;
}
for (var midiChannel = 0; midiChannel < 2; midiChannel++) {
    var noteStatus = 0x90 + midiChannel;
    for (var note = 36; note <= 94; note++) {
        if (note === 61 || note === 92 || note === 93) continue;
        assert(hasMidi(noteStatus, note), "XML missing note " + note + " on channel " + (midiChannel + 1));
    }
    assert(hasMidi(noteStatus, 100), "XML missing Browse touch on channel " + (midiChannel + 1));
    var ccStatus = 0xB0 + midiChannel;
    for (var cc = 16; cc <= 21; cc++)
        assert(hasMidi(ccStatus, cc), "XML missing encoder CC " + cc + " on channel " + (midiChannel + 1));
    for (cc = 32; cc <= 40; cc++)
        assert(hasMidi(ccStatus, cc), "XML missing slider CC " + cc + " on channel " + (midiChannel + 1));
    assert(hasMidi(ccStatus, 0x70), "XML missing Cue CC on channel " + (midiChannel + 1));
    assert(hasMidi(ccStatus, 0x71), "XML missing Play CC on channel " + (midiChannel + 1));
}

D2.shutdown();
assert(sysex.length > 100, "display/LED state publication missing");
console.log("D2_COMPLETE_CONTROLS_OK calls=" + calls.length + " sysex=" + sysex.length);
