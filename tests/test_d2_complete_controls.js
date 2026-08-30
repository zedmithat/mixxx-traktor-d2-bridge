"use strict";

var fs = require("fs");
var vm = require("vm");
var values = {};
var parameters = {};
var calls = [];
var sysex = [];
var connections = [];
var timers = [];
var scratchEvents = [];
var scratching = {};

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
    getTrackLocation: function(group) {
        return group === "[Channel1]" ?
            "/media/pi/REKORDBOX/Deck 1.mp3" :
            "/media/pi/REKORDBOX/Deck 2.mp3";
    },
    getValue: function(group, control) {
        if (values[key(group, control)] !== undefined) return values[key(group, control)];
        if (control === "duration") return 420;
        if (control === "bpm") return 128;
        if (control === "visual_bpm") return 129.5;
        if (control === "rate_ratio") return 1;
        if (control === "beat_prev") return 100;
        if (control === "beat_next") return 200;
        if (control === "track_samplerate") return 44100;
        if (control === "track_id") return group === "[Channel1]" ? 101 : 202;
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
    scratchEnable: function(deck, intervals, rpm, alpha, beta, ramp) {
        scratching[deck] = true;
        scratchEvents.push(["enable", deck, intervals, rpm, alpha, beta, ramp]);
    },
    scratchTick: function(deck, delta) {
        scratchEvents.push(["tick", deck, delta]);
    },
    scratchDisable: function(deck, ramp) {
        scratching[deck] = false;
        scratchEvents.push(["disable", deck, ramp]);
    },
    isScratching: function(deck) { return !!scratching[deck]; },
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
timers.filter(function(timer) { return timer.interval === 250; })[0].callback();

/* Remix pads address all eight sampler slots.  Loaded feedback must therefore
 * cover 1..8, while mute feedback belongs only to the four physical strips. */
for (var samplerConnection = 1; samplerConnection <= 8; samplerConnection++) {
    assert(connections.some(function(connection) {
        return connection.group === "[Sampler" + samplerConnection + "]" &&
            connection.control === "track_loaded";
    }), "Sampler " + samplerConnection + " track_loaded feedback missing");
    var hasMuteConnection = connections.some(function(connection) {
        return connection.group === "[Sampler" + samplerConnection + "]" &&
            connection.control === "mute";
    });
    assert(hasMuteConnection === (samplerConnection <= 4),
        "Sampler " + samplerConnection + " mute feedback ownership is wrong");
}

function sysexText(message) {
    return message.slice(2, message.length - 1).map(function(value) {
        return String.fromCharCode(value);
    }).join("");
}
assert(timers.some(function(timer) { return timer.interval === 500; }),
    "500 ms cached phase recovery timer missing");
assert(sysex.some(function(message) {
    return sysexText(message) === "D2|1|PHASE|0.00000,0.00000,0,0,0,1,2";
}), "deck 1 phase payload missing");
assert(sysex.some(function(message) {
    return sysexText(message) === "D2|2|PHASE|0.00000,0.00000,0,0,0,1,2";
}), "deck 2 phase payload missing");
assert(sysex.some(function(message) {
    return sysexText(message) === "D2|1|GRIDEDIT|0";
}) && sysex.some(function(message) {
    return sysexText(message) === "D2|2|GRIDEDIT|0";
}), "startup did not reset both GRIDEDIT renderer states");

/* PHASE carries validity plus explicit master/follower deck labels. Both D2s
 * must receive the same stable row ordering, including on the leader screen. */
values[key("[Channel1]", "sync_leader")] = 0;
values[key("[Channel2]", "sync_leader")] = 0;
values[key("[Channel1]", "track_loaded")] = 1;
values[key("[Channel2]", "track_loaded")] = 1;
var phaseValidStart = sysex.length;
D2.updatePhaseMeter();
var phaseNoLeader = sysex.slice(phaseValidStart).map(sysexText).filter(function(text) {
    return text.indexOf("|PHASE|") >= 0;
});
assert(phaseNoLeader.length === 2 && phaseNoLeader.every(function(text) {
    var fields = text.split("|")[3].split(",");
    return fields.length === 7 && fields[4] === "1" &&
        fields[5] === "1" && fields[6] === "2";
}), "loaded decks did not publish the same valid A-over-B PHASE rows");
values[key("[Channel2]", "track_loaded")] = 0;
var phaseInvalidStart = sysex.length;
D2.updatePhaseMeter();
assert(sysex.slice(phaseInvalidStart).some(function(message) {
    var text = sysexText(message);
    return text.indexOf("D2|1|PHASE|") === 0 &&
        text.split("|")[3].split(",")[4] === "0";
}), "unloaded comparison deck left the phase meter valid");
values[key("[Channel2]", "track_loaded")] = 1;
values[key("[Channel1]", "sync_leader")] = 1;
D2.phaseBeatStep["[Channel1]"] = 1;
D2.phaseBeatStep["[Channel2]"] = 3;
var leaderAStart = sysex.length;
D2.updatePhaseMeter();
var leaderARows = sysex.slice(leaderAStart).map(sysexText).filter(function(text) {
    return text.indexOf("|PHASE|") >= 0;
});
assert(leaderARows.length === 2 && leaderARows.every(function(text) {
    var fields = text.split("|")[3].split(",");
    return fields[2] === "1" && fields[3] === "3" &&
        fields[4] === "1" && fields[5] === "1" && fields[6] === "2";
}), "Sync Leader A was not shown on both D2 top rows");
values[key("[Channel1]", "sync_leader")] = 0;
values[key("[Channel2]", "sync_leader")] = 1;
var leaderBStart = sysex.length;
D2.updatePhaseMeter();
var leaderBRows = sysex.slice(leaderBStart).map(sysexText).filter(function(text) {
    return text.indexOf("|PHASE|") >= 0;
});
assert(leaderBRows.length === 2 && leaderBRows.every(function(text) {
    var fields = text.split("|")[3].split(",");
    return fields[2] === "3" && fields[3] === "1" &&
        fields[4] === "1" && fields[5] === "2" && fields[6] === "1";
}), "Sync Leader B did not reorder both D2 rows together");
values[key("[Channel2]", "sync_leader")] = 0;
values[key("[Channel1]", "sync_leader")] = 1;
assert(sysex.some(function(message) {
    return sysexText(message).indexOf("D2|1|KEYVISUAL|20") === 0;
}), "live visual key payload missing");
assert(sysex.some(function(message) {
    return sysexText(message) === "D2|1|TRACKID|101";
}), "deck 1 exact track ID payload missing");
assert(sysex.some(function(message) {
    return sysexText(message) === "D2|2|TRACKID|202";
}), "deck 2 exact track ID payload missing");
assert(connections.filter(function(connection) {
    return connection.control.indexOf("hotcue_") === 0 &&
           connection.control.indexOf("_position") > 0;
}).length === 16, "live Hotcue position connections missing");
assert(connections.filter(function(connection) {
    return connection.control.indexOf("hotcue_") === 0 &&
           connection.control.indexOf("_color") > 0;
}).length === 16, "live Hotcue color connections missing");
assert(connections.filter(function(connection) {
    return connection.control === "beat_active";
}).length === 2, "per-beat phase-step connections missing");
var loadRejectedConnections = connections.filter(function(connection) {
    return connection.control === "d2_load_rejected_playing_sequence";
});
assert(loadRejectedConnections.length === 2,
    "authoritative per-deck load-rejection connections missing");
var sysexBeforeLoadReject = sysex.length;
loadRejectedConnections.filter(function(connection) {
    return connection.group === "[Channel1]";
})[0].callback(1);
assert(sysex.length === sysexBeforeLoadReject + 1 &&
    sysexText(sysex[sysex.length - 1]) === "D2|1|LOADREJECT|PLAYING",
    "Mixxx playing-deck rejection was not routed to D2 surface 1");
sysexBeforeLoadReject = sysex.length;
loadRejectedConnections.filter(function(connection) {
    return connection.group === "[Channel2]";
})[0].callback(0);
assert(sysex.length === sysexBeforeLoadReject,
    "zero rejection sequence unexpectedly produced an overlay event");
[
    ["d2_load_missing_sequence", "MISSING"],
    ["d2_load_no_selection_sequence", "NOSELECTION"],
    ["d2_track_load_failed_sequence", "FAILED"]
].forEach(function(outcome) {
    var outcomeConnections = connections.filter(function(connection) {
        return connection.control === outcome[0];
    });
    assert(outcomeConnections.length === 2,
        "authoritative per-deck " + outcome[0] + " connections missing");

    var before = sysex.length;
    resetCalls();
    outcomeConnections.filter(function(connection) {
        return connection.group === "[Channel2]";
    })[0].callback(1);
    assert(sysex.length === before + 1 &&
        sysexText(sysex[sysex.length - 1]) ===
            "D2|2|LOADFAIL|" + outcome[1],
        outcome[0] + " was not routed to D2 surface 2");
    assert(!called("[Tab]", "current"),
        outcome[0] + " unexpectedly closed Browse");

    before = sysex.length;
    outcomeConnections.filter(function(connection) {
        return connection.group === "[Channel1]";
    })[0].callback(0);
    assert(sysex.length === before,
        "zero " + outcome[0] + " sequence produced an overlay event");
});
var beatConnection = connections.filter(function(connection) {
    return connection.group === "[Channel1]" &&
           connection.control === "beat_active";
})[0];
D2.phaseBeatStep["[Channel1]"] = 0;
D2.phaseBeatActive["[Channel1]"] = false;
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
var hotcueColorConnection = connections.filter(function(connection) {
    return connection.group === "[Channel1]" &&
           connection.control === "hotcue_1_color";
})[0];
values[key("[Channel1]", "hotcue_1_status")] = 1;
values[key("[Channel1]", "hotcue_1_color")] = 0x123456;
var hotcueColorStart = sysex.length;
hotcueColorConnection.callback(0x123456);
assert(sysex.slice(hotcueColorStart).some(function(message) {
    return sysexText(message) === "D2|1|CUECOLOR1|1193046";
}), "real Mixxx Hotcue color was not published to the overview renderer");
assert(D2.padColor("[Channel1]", 1) === 0x123456,
    "real Mixxx Hotcue color did not drive the physical pad color");
var coloredLedPack = sysex.map(sysexText).filter(function(text) {
    return text.indexOf("D2|1|LEDPACK|") === 0;
}).pop();
assert(coloredLedPack && coloredLedPack.split("|")[3].split(",")[12] === "123456",
    "real Mixxx Hotcue color was absent from the physical LED packet");

/* A successful native load closes both Browse surfaces. Missing/offline rows
 * never emit track_loaded and must therefore leave the library visible. */
values[key("[Tab]", "current")] = 1;
resetCalls();
var trackGridOnStart = sysex.length;
D2.editButton(0, 0x41, 0x7F, 0, "[Channel1]");
assert(D2.beatgridEdit["[Channel1]"] &&
    sysex.slice(trackGridOnStart).some(function(message) {
    return sysexText(message) === "D2|1|GRIDEDIT|1";
}), "EDIT did not publish the active GRIDEDIT state");
var trackGridResetStart = sysex.length;
D2.trackLoaded("[Channel1]", 1);
assert(called("[Tab]", "current", 0),
    "successful Browse load did not return the main screen to Player");
assert(!D2.beatgridEdit["[Channel1]"] &&
    sysex.slice(trackGridResetStart).some(function(message) {
    return sysexText(message) === "D2|1|GRIDEDIT|0";
}), "successful track load did not clear GRIDEDIT state");
values[key("[Tab]", "current")] = 1;
resetCalls();
values[key("[Channel1]", "track_loaded")] = 0;
D2.trackLoaded("[Channel1]", 0);
assert(!called("[Tab]", "current"),
    "failed/offline Browse load unexpectedly closed the library");
var deferredUnload = timers[timers.length - 1];
assert(deferredUnload.interval === 100,
    "track identity clear was not deferred for stale load protection");
deferredUnload.callback();
assert(sysex.some(function(message) {
    return sysexText(message) === "D2|1|TRACKID|0";
}), "track unload did not clear the renderer identity");
values[key("[Channel1]", "track_loaded")] = 1;
values[key("[Tab]", "current")] = 0;

var surfaces = ["[Channel1]", "[Channel2]"];
surfaces.forEach(function(surface, surfaceIndex) {
    var initialDeck = surfaceIndex + 1;
    var initialGroup = "[Channel" + initialDeck + "]";
    var surfaceNumber = surfaceIndex + 1;

    function surfacePacketsSince(start, stateName) {
        var prefix = "D2|" + surfaceNumber + "|" + stateName + "|";
        return sysex.slice(start).map(sysexText).filter(function(text) {
            return text.indexOf(prefix) === 0;
        });
    }

    function latestPadColorSince(start, pad) {
        var packets = surfacePacketsSince(start, "LEDPACK");
        assert(packets.length > 0,
            surface + " missing LEDPACK for pad color " + pad);
        var fields = packets[packets.length - 1].split("|")[3].split(",");
        return parseInt(fields[12 + pad - 1], 16);
    }

    function assertPerformanceModeButton(handler, control, mode, ledMode) {
        var beforeReleaseMode = D2.performanceMode[surface];
        var releaseStart = sysex.length;
        handler(0, control, 0, 0, surface);
        assert(D2.performanceMode[surface] === beforeReleaseMode &&
               sysex.length === releaseStart,
            surface + " " + mode + " mode reacted to button release");

        var packetStart = sysex.length;
        handler(0, control, 0x7F, 0, surface);
        assert(D2.performanceMode[surface] === mode,
            surface + " " + mode + " mode button did not select its mode");
        assert(surfacePacketsSince(packetStart, "VIEW").some(function(text) {
            return text === "D2|" + surfaceNumber + "|VIEW|" + mode;
        }), surface + " " + mode + " mode did not publish its VIEW payload");
        var ledPackets = surfacePacketsSince(packetStart, "LEDPACK");
        assert(ledPackets.length > 0 && ledPackets.some(function(text) {
            return text.split("|")[3].split(",")[7] === String(ledMode);
        }), surface + " " + mode + " mode did not publish its LEDPACK mode");
    }

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
    values[key(initialGroup, "sync_enabled")] = 0;
    D2.syncButton(0, 0x5B, 0x7F, 0, surface);
    assert(called(initialGroup, "sync_enabled", true),
        surface + " normal SYNC did not latch the live sync state");
    assert(!called(initialGroup, "beatsync"),
        surface + " normal SYNC unexpectedly used momentary beatsync");
    var syncOnLedPack = sysex.map(sysexText).filter(function(text) {
        return text.indexOf("D2|" + (surfaceIndex + 1) + "|LEDPACK|") === 0;
    }).pop();
    assert(syncOnLedPack && syncOnLedPack.split("|")[3].split(",")[2] === "1",
        surface + " latched SYNC-on state did not reach its LED packet");
    var callsBeforeSyncRelease = calls.length;
    D2.syncButton(0, 0x5B, 0, 0, surface);
    assert(calls.length === callsBeforeSyncRelease,
        surface + " SYNC release unexpectedly changed the latched state");

    resetCalls();
    D2.syncButton(0, 0x5B, 0x7F, 0, surface);
    assert(called(initialGroup, "sync_enabled", false),
        surface + " second normal SYNC press did not unlatch sync");
    var syncOffLedPack = sysex.map(sysexText).filter(function(text) {
        return text.indexOf("D2|" + (surfaceIndex + 1) + "|LEDPACK|") === 0;
    }).pop();
    assert(syncOffLedPack && syncOffLedPack.split("|")[3].split(",")[2] === "0",
        surface + " unlatched SYNC state did not clear its LED packet");

    resetCalls();
    D2.shiftButton(0, 0x5A, 0x7F, 0, surface);
    D2.syncButton(0, 0x5B, 0x7F, 0, surface);
    assert(called(initialGroup, "beatsync", 1) &&
           called(initialGroup, "beatsync", 0),
        surface + " SHIFT+SYNC did not perform one-shot beat sync");
    assert(!called(initialGroup, "sync_enabled"),
        surface + " SHIFT+SYNC unexpectedly changed the latched sync state");
    D2.shiftButton(0, 0x5A, 0, 0, surface);

    /* Traktor-style Touch Strip: touching alone is neutral, normal movement
     * is relative/inverted pitch bend, release neutralizes jog, and SHIFT is
     * absolute needle search. */
    resetCalls();
    values[key(initialGroup, "play")] = 1;
    values[key(initialGroup, "play_indicator")] = 1;
    D2.touchStrip(0, 0x28, 64, 0, surface);
    assert(!called(initialGroup, "jog"),
        surface + " touch strip moved without a touch");
    D2.touchStripTouch(0, 0x5E, 0x7F, 0, surface);
    D2.touchStrip(0, 0x28, 64, 0, surface);
    assert(!called(initialGroup, "jog"),
        surface + " first touch-strip sample caused a jump");
    D2.touchStrip(0, 0x28, 72, 0, surface);
    var rightBend = calls.filter(function(call) {
        return call[1] === initialGroup && call[2] === "jog";
    }).pop();
    assert(rightBend && rightBend[3] < 0,
        surface + " right swipe did not apply Traktor-direction bend");
    D2.touchStrip(0, 0x28, 60, 0, surface);
    var leftBend = calls.filter(function(call) {
        return call[1] === initialGroup && call[2] === "jog";
    }).pop();
    assert(leftBend && leftBend[3] > 0,
        surface + " left swipe did not apply Traktor-direction bend");
    D2.touchStripTouch(0, 0x5E, 0, 0, surface);
    assert(called(initialGroup, "jog", 0),
        surface + " touch-strip release did not neutralize jog");

    resetCalls();
    D2.shiftButton(0, 0x5A, 0x7F, 0, surface);
    D2.touchStripTouch(0, 0x5E, 0x7F, 0, surface);
    D2.touchStrip(0, 0x28, 96, 0, surface);
    var seek = calls.filter(function(call) {
        return call[1] === initialGroup && call[2] === "playposition";
    }).pop();
    assert(seek && Math.abs(seek[3] - 96 / 127) < 0.000001,
        surface + " SHIFT touch strip did not perform absolute needle search");
    D2.touchStripTouch(0, 0x5E, 0, 0, surface);
    D2.shiftButton(0, 0x5A, 0, 0, surface);

    scratchEvents.length = 0;
    values[key(initialGroup, "play")] = 0;
    values[key(initialGroup, "play_indicator")] = 0;
    D2.touchStripTouch(0, 0x5E, 0x7F, 0, surface);
    assert(scratchEvents.some(function(event) {
        return event[0] === "enable" && event[1] === initialDeck;
    }), surface + " stopped touch did not enable scratch mode");
    D2.touchStrip(0, 0x28, 64, 0, surface);
    assert(!scratchEvents.some(function(event) { return event[0] === "tick"; }),
        surface + " first scratch sample moved the deck");
    D2.touchStrip(0, 0x28, 72, 0, surface);
    D2.touchStrip(0, 0x28, 60, 0, surface);
    assert(scratchEvents.some(function(event) {
        return event[0] === "tick" && event[1] === initialDeck && event[2] === -8;
    }), surface + " right scratch movement was not routed");
    assert(scratchEvents.some(function(event) {
        return event[0] === "tick" && event[1] === initialDeck && event[2] === 12;
    }), surface + " left scratch movement was not routed");
    D2.touchStripTouch(0, 0x5E, 0, 0, surface);
    assert(scratchEvents.some(function(event) {
        return event[0] === "disable" && event[1] === initialDeck;
    }), surface + " touch release did not disable scratch mode");

    resetCalls();
    D2.loopEncoder(0, 0x15, 0x41, 0, surface);
    D2.loopEncoder(0, 0x15, 0x3F, 0, surface);
    D2.loopPress(0, 0x42, 0x7F, 0, surface);
    D2.loopTouch(0, 0x43, 0x7F, 0, surface);
    D2.loopTouch(0, 0x43, 0, 0, surface);
    assert(called(initialGroup, "loop_double", 1), surface + " loop double missing");
    assert(called(initialGroup, "loop_halve", 1), surface + " loop halve missing");
    assert(called(initialGroup, "beatloop_activate", 1), surface + " loop press missing");

    /* Loop-mode pads are latched loop toggles, not momentary activate pulses.
     * Pressing an already active size must therefore turn that loop off. */
    D2.setPerformanceMode(surface, "LOOP");
    [0.25, 0.5, 1, 2, 4, 8, 16, 32].forEach(function(beats, loopPad) {
        resetCalls();
        D2.padButton(0, 0x4C + loopPad, 0x7F, 0, surface);
        assert(called(initialGroup, "beatloop_" + beats + "_toggle", 1) &&
               called(initialGroup, "beatloop_" + beats + "_toggle", 0),
            surface + " loop pad " + (loopPad + 1) + " is not a toggle");
    });

    /* The four physical performance-mode buttons own both the rendered view
     * and the LED mode field. Releases must be inert, otherwise one physical
     * press can produce two view changes on controllers that report both
     * edges. Exercise this contract independently on both D2 surfaces. */
    assertPerformanceModeButton(D2.hotcueButton, 0x54, "HOTCUE", 1);
    assertPerformanceModeButton(D2.loopButton, 0x55, "LOOP", 2);
    assertPerformanceModeButton(D2.freezeButton, 0x56, "FREEZE", 3);
    assertPerformanceModeButton(D2.remixButton, 0x57, "SAMPLER", 4);

    /* Pad edge semantics are mode-specific: Hotcue and Roll are held actions,
     * while Loop, Beatjump and Sampler are press-only pulses. A release may
     * never retrigger a press-only action. */
    D2.shiftPressed[surface] = false;
    D2.setPerformanceMode(surface, "HOTCUE");
    resetCalls();
    D2.padButton(0, 0x4C, 0x7F, 0, surface);
    D2.padButton(0, 0x4C, 0, 0, surface);
    var hotcueEdges = calls.filter(function(call) {
        return call[1] === initialGroup &&
               call[2] === "hotcue_1_activate";
    });
    assert(hotcueEdges.length === 2 && hotcueEdges[0][3] === 1 &&
           hotcueEdges[1][3] === 0,
        surface + " Hotcue pad did not preserve press/release hold semantics");

    D2.setPerformanceMode(surface, "FREEZE");
    resetCalls();
    var rollPressPackets = sysex.length;
    D2.padButton(0, 0x4D, 0x7F, 0, surface);
    assert(latestPadColorSince(rollPressPackets, 2) === 0x4080FF,
        surface + " held Roll pad did not publish bright screen/LED color");
    var rollReleasePackets = sysex.length;
    D2.padButton(0, 0x4D, 0, 0, surface);
    assert(latestPadColorSince(rollReleasePackets, 2) === 0x102048,
        surface + " released Roll pad did not return to its idle color");
    var rollEdges = calls.filter(function(call) {
        return call[1] === initialGroup &&
               call[2] === "beatlooproll_0.5_activate";
    });
    assert(rollEdges.length === 2 && rollEdges[0][3] === 1 &&
           rollEdges[1][3] === 0,
        surface + " Roll pad did not preserve press/release hold semantics");

    D2.setPerformanceMode(surface, "LOOP");
    values[key(initialGroup, "beatloop_size")] = 8;
    values[key(initialGroup, "loop_enabled")] = 0;
    var loopIdlePackets = sysex.length;
    D2.refreshLEDs(surface);
    assert(latestPadColorSince(loopIdlePackets, 6) === 0x087030,
        surface + " selected inactive 8-beat Loop pad color is wrong");
    values[key(initialGroup, "loop_enabled")] = 1;
    var loopActivePackets = sysex.length;
    D2.refreshLEDs(surface);
    assert(latestPadColorSince(loopActivePackets, 6) === 0x00FF60,
        surface + " active 8-beat Loop pad did not become bright green");
    resetCalls();
    D2.padButton(0, 0x4E, 0, 0, surface);
    assert(calls.length === 0,
        surface + " Loop pad release retriggered the loop");
    D2.padButton(0, 0x4E, 0x7F, 0, surface);
    var loopPulses = calls.filter(function(call) {
        return call[1] === initialGroup &&
               call[2] === "beatloop_1_toggle";
    });
    assert(loopPulses.length === 2 && loopPulses[0][3] === 1 &&
           loopPulses[1][3] === 0,
        surface + " Loop pad did not emit exactly one press pulse");

    var jumpIdlePackets = sysex.length;
    D2.setPerformanceMode(surface, "BEATJUMP");
    assert(latestPadColorSince(jumpIdlePackets, 1) === 0x402000,
        surface + " idle Beatjump pad color is wrong");
    resetCalls();
    D2.padButton(0, 0x4C, 0, 0, surface);
    assert(calls.length === 0,
        surface + " Beatjump pad release retriggered the jump");
    var jumpPressPackets = sysex.length;
    D2.padButton(0, 0x4C, 0x7F, 0, surface);
    assert(called(initialGroup, "beatjump_1_backward", 1) &&
           called(initialGroup, "beatjump_1_backward", 0),
        surface + " Beatjump pad did not emit its backward pulse");
    assert(latestPadColorSince(jumpPressPackets, 1) === 0xFFB000,
        surface + " pressed Beatjump pad did not become bright amber");
    var jumpReleasePackets = sysex.length;
    D2.padButton(0, 0x4C, 0, 0, surface);
    assert(latestPadColorSince(jumpReleasePackets, 1) === 0x402000,
        surface + " released Beatjump pad did not return to dark amber");

    D2.setPerformanceMode(surface, "SAMPLER");
    resetCalls();
    D2.padButton(0, 0x4C, 0, 0, surface);
    assert(calls.length === 0,
        surface + " Sampler pad release retriggered playback");
    D2.padButton(0, 0x4C, 0x7F, 0, surface);
    assert(called("[Sampler1]", "start_play", 1) &&
           called("[Sampler1]", "start_play", 0),
        surface + " Sampler pad did not emit exactly one start pulse");
    resetCalls();
    D2.shiftPressed[surface] = true;
    D2.padButton(0, 0x4C, 0x7F, 0, surface);
    assert(called("[Sampler1]", "cue_gotoandstop", 1) &&
           called("[Sampler1]", "cue_gotoandstop", 0),
        surface + " SHIFT Sampler pad did not stop/return the sampler");
    D2.shiftPressed[surface] = false;

    /* Left screen controls: zoom has exactly three states, time exactly two;
     * Key Lock is a state toggle and Key Sync is a single press pulse. */
    D2.zoomLevel[surface] = 2;
    var zoomStart = sysex.length;
    D2.leftScreenButton(0, 0x31, 0, 0, surface);
    assert(D2.zoomLevel[surface] === 2 && sysex.length === zoomStart,
        surface + " Zoom reacted to button release");
    D2.leftScreenButton(0, 0x31, 0x7F, 0, surface);
    D2.leftScreenButton(0, 0x31, 0x7F, 0, surface);
    D2.leftScreenButton(0, 0x31, 0x7F, 0, surface);
    assert(D2.zoomLevel[surface] === 2,
        surface + " Zoom did not cycle 2x -> 4x -> 8x -> 2x");
    var zoomPayloads = surfacePacketsSince(zoomStart, "ZOOM");
    assert(zoomPayloads.length === 3 &&
           zoomPayloads[0].slice(-1) === "4" &&
           zoomPayloads[1].slice(-1) === "8" &&
           zoomPayloads[2].slice(-1) === "2",
        surface + " Zoom payload sequence is not 4x, 8x, 2x");

    values[key(initialGroup, "keylock")] = 0;
    resetCalls();
    D2.leftScreenButton(0, 0x32, 0x7F, 0, surface);
    assert(called(initialGroup, "keylock", true),
        surface + " Key Lock screen button did not toggle on");
    var keylockCallCount = calls.length;
    D2.leftScreenButton(0, 0x32, 0, 0, surface);
    assert(calls.length === keylockCallCount,
        surface + " Key Lock screen button reacted to release");

    resetCalls();
    D2.leftScreenButton(0, 0x33, 0x7F, 0, surface);
    assert(called(initialGroup, "sync_key", 1) &&
           called(initialGroup, "sync_key", 0),
        surface + " Key Sync screen button did not emit one pulse");

    D2.timeMode[surface] = 0;
    var timeModeStart = sysex.length;
    D2.leftScreenButton(0, 0x34, 0x7F, 0, surface);
    D2.leftScreenButton(0, 0x34, 0x7F, 0, surface);
    D2.leftScreenButton(0, 0x34, 0x7F, 0, surface);
    assert(D2.timeMode[surface] === 1,
        surface + " Time mode escaped the two-state elapsed/remain cycle");
    var timePayloads = surfacePacketsSince(timeModeStart, "TIMEMODE");
    assert(timePayloads.length === 3 &&
           timePayloads[0].slice(-1) === "1" &&
           timePayloads[1].slice(-1) === "0" &&
           timePayloads[2].slice(-1) === "1",
        surface + " Time payload sequence is not elapsed/remain only");

    /* Right screen controls duplicate the immediate Hotcue/Loop selectors,
     * add Beatjump, and reserve the fourth key for Quantize. */
    [[0x35, "HOTCUE", 1], [0x36, "LOOP", 2],
     [0x37, "BEATJUMP", 5]].forEach(function(modeButton) {
        var packetStart = sysex.length;
        D2.rightScreenButton(0, modeButton[0], 0x7F, 0, surface);
        assert(D2.performanceMode[surface] === modeButton[1],
            surface + " right screen did not select " + modeButton[1]);
        assert(surfacePacketsSince(packetStart, "VIEW").some(function(text) {
            return text === "D2|" + surfaceNumber + "|VIEW|" + modeButton[1];
        }), surface + " right " + modeButton[1] + " omitted VIEW payload");
        assert(surfacePacketsSince(packetStart, "LEDPACK").some(function(text) {
            return text.split("|")[3].split(",")[7] === String(modeButton[2]);
        }), surface + " right " + modeButton[1] + " omitted LEDPACK mode");
    });
    var modeBeforeRelease = D2.performanceMode[surface];
    D2.rightScreenButton(0, 0x37, 0, 0, surface);
    assert(D2.performanceMode[surface] === modeBeforeRelease,
        surface + " right screen mode reacted to release");

    values[key(initialGroup, "quantize")] = 0;
    resetCalls();
    D2.rightScreenButton(0, 0x38, 0x7F, 0, surface);
    assert(called(initialGroup, "quantize", true),
        surface + " Quantize screen button did not toggle on");
    var quantizeCallCount = calls.length;
    D2.rightScreenButton(0, 0x38, 0, 0, surface);
    assert(calls.length === quantizeCallCount,
        surface + " Quantize screen button reacted to release");

    resetCalls();
    D2.captureButton(0, 0x40, 0, 0, surface);
    assert(calls.length === 0,
        surface + " CAPTURE reacted to button release");
    D2.captureButton(0, 0x40, 0x7F, 0, surface);
    var capturePulse = calls.filter(function(call) {
        return call[1] === "[Library]" &&
               call[2] === "AutoDjAddBottom";
    });
    assert(capturePulse.length === 2 && capturePulse[0][3] === 1 &&
           capturePulse[1][3] === 0,
        surface + " CAPTURE did not emit one AutoDjAddBottom pulse");

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
    var ownedUnit = surfaceIndex + 1;
    assert(called(D2.effectUnitGroup(surface),
        "group_[Channel" + ownedUnit + "]_enable"),
        surface + " own FX deck assignment missing");
    assert(!called(D2.effectUnitGroup(surface),
        "group_[Channel" + (ownedUnit === 1 ? 2 : 1) + "]_enable"),
        surface + " changed the other deck FX assignment");
    var oldUnit = D2.fxUnit[surface];
    D2.fxSelectButton(0, 0x30, 0x7F, 0, surface);
    assert(D2.fxUnit[surface] === oldUnit,
        surface + " FX SELECT changed fixed FX unit ownership");

    resetCalls();
    values[key("[Tab]", "current")] = D2.skinPage.browse;
    var deckGridOnStart = sysex.length;
    D2.editButton(0, 0x41, 0x7F, 0, surface);
    assert(D2.beatgridEdit[surface] &&
        sysex.slice(deckGridOnStart).some(function(message) {
        return sysexText(message) ===
            "D2|" + (surfaceIndex + 1) + "|GRIDEDIT|1";
    }), surface + " EDIT did not publish GRIDEDIT=1");
    var deckGridResetStart = sysex.length;
    D2.deckButton(0, 0x59, 0x7F, 0, surface);
    assert(D2.activeDeck[surface] === initialDeck,
        surface + " deck button changed two-deck routing");
    assert(!D2.beatgridEdit[surface] &&
        sysex.slice(deckGridResetStart).some(function(message) {
        return sysexText(message) ===
            "D2|" + (surfaceIndex + 1) + "|GRIDEDIT|0";
    }), surface + " deck button did not reset GRIDEDIT");
    assert(called("[Tab]", "current", D2.skinPage.player),
        surface + " deck button did not close the zed Browse page");
    assert(sysex.some(function(message) {
        return sysexText(message) ===
            "D2|" + (surfaceIndex + 1) + "|VIEW|DECK";
    }), surface + " deck button did not restore its D2 Player view");
});

var xml = fs.readFileSync(
    __dirname + "/../mixxx-controller/Traktor-Kontrol-D2.midi.xml", "utf8");

/* Browse press is emitted by the bridge as Note 62. Keep one authoritative
 * native LoadSelectedTrack binding per physical D2: a duplicate script
 * binding would dispatch the same press twice and can race Browse/view state. */
var xmlControlBlocks = [];
var xmlControlPattern = /<control>[\s\S]*?<\/control>/g;
var xmlControlMatch;
while ((xmlControlMatch = xmlControlPattern.exec(xml)) !== null)
    xmlControlBlocks.push(xmlControlMatch[0]);

function controlHasTag(block, tag, value) {
    return block.indexOf("<" + tag + ">" + value + "</" + tag + ">") >= 0;
}

for (var loadChannel = 0; loadChannel < 2; ++loadChannel) {
    var loadStatus = "0x9" + loadChannel;
    var loadGroup = "[Channel" + (loadChannel + 1) + "]";
    var note61Bindings = xmlControlBlocks.filter(function(block) {
        return controlHasTag(block, "status", loadStatus) &&
            controlHasTag(block, "midino", "0x3D");
    });
    assert(note61Bindings.length === 1,
        "sidebar Note 61 must have exactly one binding on MIDI channel " +
        (loadChannel + 1));
    assert(controlHasTag(note61Bindings[0], "group", loadGroup) &&
           controlHasTag(note61Bindings[0], "key", "D2.sidebarActivate") &&
           note61Bindings[0].indexOf("<script-binding/>") >= 0 &&
           note61Bindings[0].indexOf("<normal/>") < 0,
        "sidebar Note 61 is not the dedicated script path on MIDI channel " +
        (loadChannel + 1));

    var note62Bindings = xmlControlBlocks.filter(function(block) {
        return controlHasTag(block, "status", loadStatus) &&
            controlHasTag(block, "midino", "0x3E");
    });
    assert(note62Bindings.length === 1,
        "Note 62 must have exactly one binding on MIDI channel " +
        (loadChannel + 1));
    assert(controlHasTag(note62Bindings[0], "group", loadGroup),
        "Note 62 targets the wrong deck on MIDI channel " + (loadChannel + 1));
    assert(controlHasTag(note62Bindings[0], "key", "LoadSelectedTrack"),
        "Note 62 is not the native LoadSelectedTrack binding on MIDI channel " +
        (loadChannel + 1));
    assert(note62Bindings[0].indexOf("<normal/>") >= 0 &&
           note62Bindings[0].indexOf("<script-binding/>") < 0,
        "Note 62 must remain native and must not be duplicated as a JS binding " +
        "on MIDI channel " + (loadChannel + 1));

    var note100Bindings = xmlControlBlocks.filter(function(block) {
        return controlHasTag(block, "status", loadStatus) &&
            controlHasTag(block, "midino", "0x64");
    });
    assert(note100Bindings.length === 1,
        "Browse touch Note 100 must have exactly one authoritative binding " +
        "on MIDI channel " + (loadChannel + 1));
    assert(controlHasTag(note100Bindings[0], "group", loadGroup) &&
           controlHasTag(note100Bindings[0], "key", "D2.browseTouch") &&
           note100Bindings[0].indexOf("<script-binding/>") >= 0 &&
           note100Bindings[0].indexOf("<normal/>") < 0,
        "Browse touch Note 100 must be owned only by D2.browseTouch on MIDI " +
        "channel " + (loadChannel + 1));

    var note101Bindings = xmlControlBlocks.filter(function(block) {
        return controlHasTag(block, "status", loadStatus) &&
            controlHasTag(block, "midino", "0x65");
    });
    assert(note101Bindings.length === 0,
        "Dead Browse Note 101 binding must not remain on MIDI channel " +
        (loadChannel + 1));
}

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

/* Touch sensors with no renderer consumer are local state only. Publishing
 * dead packets here wastes the same MIDI/USB queue needed by physical input. */
["FADERTOUCH", "ENCTOUCH", "LOOPTOUCH"].forEach(function(deadState) {
    assert(!sysex.some(function(message) {
        return sysexText(message).indexOf("|" + deadState + "|") >= 0;
    }), deadState + " emitted an unconsumed SysEx packet");
});

/* Shutdown must leave no momentary audio action latched and must explicitly
 * blank both hardware surfaces. Use different decks for Hotcue and Roll so
 * both modes can be physically held at the same time. */
values[key("[Channel1]", "play")] = 0;
values[key("[Channel1]", "playposition")] = 0.25;
values[key("[Channel1]", "cue_point")] = 22050;
values[key("[Channel1]", "track_samples")] = 88200;
D2.cueButton(0, 0x5C, 0x7F, 0, "[Channel1]");
D2.setPerformanceMode("[Channel1]", "HOTCUE");
D2.padButton(0, 0x4C, 0x7F, 0, "[Channel1]");
D2.setPerformanceMode("[Channel2]", "FREEZE");
D2.padButton(0, 0x4D, 0x7F, 0, "[Channel2]");
resetCalls();
var shutdownSysexStart = sysex.length;
D2.shutdown();
assert(called("[Channel1]", "cue_preview", 0),
    "shutdown did not release an active CUE preview");
assert(called("[Channel1]", "hotcue_1_activate", 0),
    "shutdown did not release a held Hotcue pad");
assert(called("[Channel2]", "beatlooproll_0.5_activate", 0),
    "shutdown did not release a held Roll pad");
assert(sysex.slice(shutdownSysexStart).some(function(message) {
    return sysexText(message) === "D2|1|LEDOFF|1";
}) && sysex.slice(shutdownSysexStart).some(function(message) {
    return sysexText(message) === "D2|2|LEDOFF|1";
}), "shutdown did not send LEDOFF to both D2 surfaces");
assert(sysex.length > 100, "display/LED state publication missing");
console.log("D2_COMPLETE_CONTROLS_OK calls=" + calls.length + " sysex=" + sysex.length);
