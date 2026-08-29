/*
 * Native Instruments Traktor Kontrol D2 controller script for Mixxx 2.5.
 *
 * The script is intentionally self-contained.  It uses the normal Mixxx
 * engine/midi API and keeps all transport, library, pad, sampler, FX and LED
 * code behind small helpers.  A physical D2 normally exposes Channel1/2;
 * currentDeck is still four-deck aware so the same handlers can be reused by
 * Channel3/4 mappings.
 */
var D2Comprehensive = {};

D2Comprehensive.decks = ["[Channel1]", "[Channel2]", "[Channel3]", "[Channel4]"];
D2Comprehensive.currentDeck = 1;
D2Comprehensive.shiftPressed = false;
D2Comprehensive.padMode = "HOTCUE";
D2Comprehensive.blinkPhase = false;
D2Comprehensive.timer = null;
D2Comprehensive.connections = [];

/* D2 note/CC numbers used by the supplied MIDI preset. */
D2Comprehensive.LED = {
    play: 0x5D, cue: 0x5C, sync: 0x5B, flux: 0x58,
    loop: 0x55, browse: 0x64, pads: 0x4C,
    fxButtons: 0x28, samplerMute: 0x44
};

D2Comprehensive.deckFromGroup = function(group) {
    var match = /^\[Channel([1-4])\]$/.exec(String(group || ""));
    return match ? Number(match[1]) : D2Comprehensive.currentDeck;
};

D2Comprehensive.groupForDeck = function(deck) {
    var n = Math.max(1, Math.min(4, Number(deck) || 1));
    return "[Channel" + n + "]";
};

D2Comprehensive.activeGroup = function(group) {
    /* Inputs always target the selected deck; the mapping group is only
       context supplied by Mixxx and must not defeat deck switching. */
    return D2Comprehensive.groupForDeck(D2Comprehensive.currentDeck);
};

D2Comprehensive.safeGet = function(group, key, fallback) {
    var value = Number(engine.getValue(group, key));
    return isFinite(value) ? value : (fallback || 0);
};

D2Comprehensive.setValue = function(group, key, value) {
    engine.setValue(group, key, value);
};

D2Comprehensive.toggle = function(group, key) {
    var next = D2Comprehensive.safeGet(group, key, 0) ? 0 : 1;
    D2Comprehensive.setValue(group, key, next);
    return next;
};

D2Comprehensive.pulse = function(group, key, value) {
    D2Comprehensive.setValue(group, key, value === undefined ? 1 : value);
    D2Comprehensive.setValue(group, key, 0);
};

D2Comprehensive.relativeDirection = function(value) {
    /* D2 bridge convention is 0x41 clockwise / 0x3F counter-clockwise.
       Accept the usual signed-MIDI ranges as a fallback for other mappings. */
    if (value === 0x41) return 1;
    if (value === 0x3F) return -1;
    if (value >= 1 && value < 0x3F) return 1;
    if (value > 0x41 && value <= 127) return -1;
    return 0;
};

D2Comprehensive.colorMapper = function(state) {
    if (state === true || state === 1 || state === "on" || state === "active")
        return 127;
    if (state === "playing") return 100;
    if (state === "cue") return 80;
    if (state === "hotcue") return 64;
    if (state === "dim" || state === "loaded") return 20;
    if (state === "red") return 15;
    if (state === "yellow") return 90;
    return 0;
};

D2Comprehensive.sendLED = function(group, control, velocity) {
    /* The bridge forwards channelized MIDI to the corresponding D2. */
    if (typeof midi === "undefined" ||
        typeof midi.sendShortMsg !== "function") return;
    var deck = D2Comprehensive.deckFromGroup(group);
    midi.sendShortMsg(0x90 | ((deck - 1) & 0x0F), control & 0x7F,
                      Math.max(0, Math.min(127, Math.round(velocity))));
};

D2Comprehensive.setPadLED = function(group, pad, velocity) {
    D2Comprehensive.sendLED(group, D2Comprehensive.LED.pads + pad - 1,
                            velocity);
};

D2Comprehensive.fxGroup = function(group, slot) {
    var deck = D2Comprehensive.deckFromGroup(group);
    return "[EffectRack1_EffectUnit" + deck + "_Effect" + slot + "]";
};

D2Comprehensive.samplerGroup = function(slot) {
    return "[Sampler" + slot + "]";
};

D2Comprehensive.selectDeck = function(deck) {
    D2Comprehensive.currentDeck = Math.max(1, Math.min(4, Number(deck) || 1));
    D2Comprehensive.refreshLEDs(D2Comprehensive.activeGroup());
};

D2Comprehensive.refreshLEDs = function(group) {
    var active = D2Comprehensive.activeGroup(group);
    var playing = D2Comprehensive.safeGet(active, "play_indicator",
                                           D2Comprehensive.safeGet(active, "play"));
    var cue = D2Comprehensive.safeGet(active, "cue_indicator",
                                      D2Comprehensive.safeGet(active, "cue_default"));
    var sync = D2Comprehensive.safeGet(active, "beatsync");
    var flux = D2Comprehensive.safeGet(active, "slip_enabled");
    var loop = D2Comprehensive.safeGet(active, "beatloop_activate");
    var duration = D2Comprehensive.safeGet(active, "duration");
    var position = D2Comprehensive.safeGet(active, "playposition");
    var nearEnd = playing && duration > 0 &&
        duration * Math.max(0, 1 - position) <= 10;

    D2Comprehensive.sendLED(active, D2Comprehensive.LED.play,
                            nearEnd && D2Comprehensive.blinkPhase ? 0 :
                            D2Comprehensive.colorMapper(playing));
    D2Comprehensive.sendLED(active, D2Comprehensive.LED.cue,
                            D2Comprehensive.colorMapper(cue ? "cue" : 0));
    D2Comprehensive.sendLED(active, D2Comprehensive.LED.sync,
                            D2Comprehensive.colorMapper(sync));
    D2Comprehensive.sendLED(active, D2Comprehensive.LED.flux,
                            D2Comprehensive.colorMapper(flux));
    D2Comprehensive.sendLED(active, D2Comprehensive.LED.loop,
                            D2Comprehensive.colorMapper(loop ? "active" : 0));

    for (var pad = 1; pad <= 8; ++pad) {
        var enabled = D2Comprehensive.safeGet(active,
            "hotcue_" + pad + "_enabled");
        var velocity = D2Comprehensive.padMode === "HOTCUE" && enabled ?
            D2Comprehensive.colorMapper("hotcue") : 0;
        D2Comprehensive.setPadLED(active, pad, velocity);
    }

    for (var stem = 1; stem <= 4; ++stem) {
        var sampler = D2Comprehensive.samplerGroup(stem);
        var muted = D2Comprehensive.safeGet(sampler, "mute");
        D2Comprehensive.sendLED(active, D2Comprehensive.LED.samplerMute + stem - 1,
                                muted ? D2Comprehensive.colorMapper("red") :
                                D2Comprehensive.colorMapper("active"));
    }

    for (var fx = 1; fx <= 3; ++fx) {
        var fxOn = D2Comprehensive.safeGet(D2Comprehensive.fxGroup(active, fx),
                                          "enabled");
        D2Comprehensive.sendLED(active, D2Comprehensive.LED.fxButtons + fx - 1,
                                fxOn ? D2Comprehensive.colorMapper("yellow") : 0);
    }
};

D2Comprehensive.blinkTick = function() {
    D2Comprehensive.blinkPhase = !D2Comprehensive.blinkPhase;
    D2Comprehensive.refreshLEDs(D2Comprehensive.activeGroup());
};

D2Comprehensive.connect = function(group, key) {
    var connection = engine.makeConnection(group, key, function() {
        D2Comprehensive.refreshLEDs(group);
    });
    if (connection) D2Comprehensive.connections.push(connection);
};

D2Comprehensive.init = function() {
    D2Comprehensive.currentDeck = 1;
    D2Comprehensive.shiftPressed = false;
    D2Comprehensive.padMode = "HOTCUE";
    D2Comprehensive.connections = [];
    for (var d = 1; d <= 4; ++d) {
        var group = D2Comprehensive.groupForDeck(d);
        ["play_indicator", "play", "cue_indicator", "cue_default",
         "track_loaded", "duration", "playposition",
         "beatsync", "slip_enabled", "beatloop_activate"].forEach(function(key) {
            D2Comprehensive.connect(group, key);
        });
        for (var pad = 1; pad <= 8; ++pad)
            D2Comprehensive.connect(group, "hotcue_" + pad + "_enabled");
    }
    for (var slot = 1; slot <= 4; ++slot)
        D2Comprehensive.connect(D2Comprehensive.samplerGroup(slot), "mute");
    for (var fx = 1; fx <= 3; ++fx)
        for (var deck = 1; deck <= 4; ++deck)
            D2Comprehensive.connect(D2Comprehensive.fxGroup(
                D2Comprehensive.groupForDeck(deck), fx), "enabled");
    D2Comprehensive.timer = engine.beginTimer(250,
                                              D2Comprehensive.blinkTick);
    D2Comprehensive.refreshLEDs();
};

D2Comprehensive.shutdown = function() {
    if (D2Comprehensive.timer) engine.stopTimer(D2Comprehensive.timer);
    D2Comprehensive.timer = null;
    for (var i = 0; i < D2Comprehensive.connections.length; ++i)
        if (D2Comprehensive.connections[i] &&
            D2Comprehensive.connections[i].disconnect)
            D2Comprehensive.connections[i].disconnect();
    D2Comprehensive.connections = [];
    for (var deck = 1; deck <= 4; ++deck) {
        var group = D2Comprehensive.groupForDeck(deck);
        [D2Comprehensive.LED.play, D2Comprehensive.LED.cue,
         D2Comprehensive.LED.sync, D2Comprehensive.LED.flux,
         D2Comprehensive.LED.loop].forEach(function(control) {
            D2Comprehensive.sendLED(group, control, 0);
        });
        for (var pad = 1; pad <= 8; ++pad)
            D2Comprehensive.setPadLED(group, pad, 0);
        for (var stem = 1; stem <= 4; ++stem)
            D2Comprehensive.sendLED(group,
                D2Comprehensive.LED.samplerMute + stem - 1, 0);
        for (var fx = 1; fx <= 3; ++fx)
            D2Comprehensive.sendLED(group,
                D2Comprehensive.LED.fxButtons + fx - 1, 0);
    }
};

/* Transport and deck selection. */
D2Comprehensive.play = function(channel, control, value, status, group) {
    if (value) D2Comprehensive.toggle(D2Comprehensive.activeGroup(group), "play");
};
D2Comprehensive.cue = function(channel, control, value, status, group) {
    if (value) D2Comprehensive.pulse(D2Comprehensive.activeGroup(group), "cue_default");
};
D2Comprehensive.sync = function(channel, control, value, status, group) {
    if (value) D2Comprehensive.toggle(D2Comprehensive.activeGroup(group), "beatsync");
};
D2Comprehensive.flux = function(channel, control, value, status, group) {
    if (value) D2Comprehensive.toggle(D2Comprehensive.activeGroup(group), "slip_enabled");
};
D2Comprehensive.shift = function(channel, control, value) {
    D2Comprehensive.shiftPressed = value === 0x7F || value === 1;
};
D2Comprehensive.deckSelect = function(channel, control, value) {
    if (value) D2Comprehensive.selectDeck((control & 0x03) + 1);
};

D2Comprehensive.touchStrip = function(channel, control, value, status, group) {
    var active = D2Comprehensive.activeGroup(group);
    var normalized = Math.max(0, Math.min(1, value / 127));
    if (D2Comprehensive.shiftPressed)
        D2Comprehensive.setValue(active, "playposition", normalized);
    else
        D2Comprehensive.setValue(active, "jog", (normalized - 0.5) * 6);
};

/* Browser and load controls. */
D2Comprehensive.browse = function(channel, control, value, status, group) {
    var direction = D2Comprehensive.relativeDirection(value);
    if (!direction) return;
    var steps = D2Comprehensive.shiftPressed ? 8 : 1;
    var key = direction > 0 ? "MoveDown" : "MoveUp";
    for (var i = 0; i < steps; ++i) D2Comprehensive.pulse("[Library]", key);
};
D2Comprehensive.load = function(channel, control, value, status, group) {
    if (value) D2Comprehensive.pulse(D2Comprehensive.activeGroup(group),
                                     "LoadSelectedTrack");
};
D2Comprehensive.back = function(channel, control, value) {
    if (value) D2Comprehensive.pulse("[Library]", "GoToSidebar");
};
D2Comprehensive.view = function(channel, control, value) {
    if (value) D2Comprehensive.pulse("[Skin]", "toggle_main_elements");
};

/* Loop and beat jump encoders/buttons. */
D2Comprehensive.loopEncoder = function(channel, control, value, status, group) {
    var active = D2Comprehensive.activeGroup(group);
    var direction = D2Comprehensive.relativeDirection(value);
    if (direction > 0) D2Comprehensive.pulse(active, "loop_double");
    else if (direction < 0) D2Comprehensive.pulse(active, "loop_halve");
};
D2Comprehensive.loopButton = function(channel, control, value, status, group) {
    if (value) D2Comprehensive.toggle(D2Comprehensive.activeGroup(group),
                                      "beatloop_activate");
};
D2Comprehensive.beatJumpEncoder = function(channel, control, value, status, group) {
    var active = D2Comprehensive.activeGroup(group);
    var direction = D2Comprehensive.relativeDirection(value);
    if (direction > 0) D2Comprehensive.pulse(active, "beatjump_forward");
    else if (direction < 0) D2Comprehensive.pulse(active, "beatjump_backward");
};
D2Comprehensive.beatJumpButton = function(channel, control, value, status, group) {
    if (value) D2Comprehensive.pulse(D2Comprehensive.activeGroup(group),
                                     "beatjump_size");
};

/* FX controls.  Knobs 1..3 are parameters 1..3; knob 4 is unit mix. */
D2Comprehensive.fxKnob = function(channel, control, value, status, group) {
    var active = D2Comprehensive.activeGroup(group);
    var normalized = Math.max(0, Math.min(1, value / 127));
    var knob = control - 0x24;
    var unit = "[EffectRack1_EffectUnit" +
        D2Comprehensive.deckFromGroup(active) + "]";
    if (knob >= 0 && knob <= 2)
        engine.setParameter(unit, "parameter" + (knob + 1), normalized);
    else if (knob === 3)
        engine.setParameter(unit, "mix", normalized);
};
D2Comprehensive.fxButton = function(channel, control, value, status, group) {
    if (!value) return;
    var active = D2Comprehensive.activeGroup(group);
    var slot = control - D2Comprehensive.LED.fxButtons;
    if (slot >= 0 && slot < 3)
        D2Comprehensive.toggle(D2Comprehensive.fxGroup(active, slot + 1), "enabled");
    else if (slot === 3)
        D2Comprehensive.toggle(active,
                               "group_[Channel" + D2Comprehensive.deckFromGroup(active) + "]_enable");
};

/* Performance pads: Hotcue, loop, or Remix/Sampler mode. */
D2Comprehensive.setPadMode = function(mode) {
    D2Comprehensive.padMode = mode;
    D2Comprehensive.refreshLEDs();
};
D2Comprehensive.pad = function(channel, control, value, status, group) {
    if (!value) return;
    var pad = control - D2Comprehensive.LED.pads + 1;
    if (pad < 1 || pad > 8) return;
    var active = D2Comprehensive.activeGroup(group);
    if (D2Comprehensive.padMode === "HOTCUE") {
        var key = "hotcue_" + pad + (D2Comprehensive.shiftPressed ? "_clear" : "_activate");
        D2Comprehensive.pulse(active, key);
    } else if (D2Comprehensive.padMode === "LOOP") {
        /* Keep the pad contract stable for custom Mixxx controls. */
        D2Comprehensive.pulse(active, "beatloop_" + pad + "_toggle");
    } else if (D2Comprehensive.padMode === "REMIX") {
        D2Comprehensive.pulse(D2Comprehensive.samplerGroup(pad), "start_play");
    }
};

/* Sampler faders, mute buttons and pre-gain knobs. */
D2Comprehensive.samplerFader = function(channel, control, value) {
    var slot = control - 0x20 + 1;
    if (slot >= 1 && slot <= 4)
        engine.setParameter(D2Comprehensive.samplerGroup(slot), "volume",
                            Math.max(0, Math.min(1, value / 127)));
};
D2Comprehensive.samplerMute = function(channel, control, value) {
    var slot = control - D2Comprehensive.LED.samplerMute + 1;
    if (value && slot >= 1 && slot <= 4)
        D2Comprehensive.toggle(D2Comprehensive.samplerGroup(slot), "mute");
};
D2Comprehensive.samplerKnob = function(channel, control, value) {
    var slot = control - 0x30 + 1;
    if (slot >= 1 && slot <= 4)
        engine.setParameter(D2Comprehensive.samplerGroup(slot), "pre_gain",
                            Math.max(0, Math.min(1, value / 127)));
};

/* Compatibility aliases for Mixxx presets using the conventional D2 prefix.
   This lets an XML mapping migrate incrementally without renaming handlers. */
var D2 = D2Comprehensive;
D2.shiftButton = D2Comprehensive.shift;
D2.syncButton = D2Comprehensive.sync;
D2.fluxButton = D2Comprehensive.flux;
D2.browseEncoder = D2Comprehensive.browse;
D2.loadSelectedTrack = D2Comprehensive.load;
D2.backButton = D2Comprehensive.back;
D2.viewButton = D2Comprehensive.view;
D2.loopEncoder = D2Comprehensive.loopEncoder;
D2.loopButton = D2Comprehensive.loopButton;
D2.beatJumpEncoder = D2Comprehensive.beatJumpEncoder;
D2.beatJumpButton = D2Comprehensive.beatJumpButton;
D2.padButton = D2Comprehensive.pad;
D2.setPerformanceMode = function(group, mode) {
    D2Comprehensive.setPadMode(mode);
};
/* Mixxx invokes these lifecycle names at script scope. */
function init(id, debugging) {
    D2Comprehensive.init(id, debugging);
}
function shutdown() {
    D2Comprehensive.shutdown();
}
function refreshLEDs(group) {
    D2Comprehensive.refreshLEDs(group);
}
var D2ComprehensiveController = D2Comprehensive;
