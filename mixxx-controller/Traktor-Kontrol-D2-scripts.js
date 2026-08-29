var D2 = function () {};

D2.shiftPressed = {"[Channel1]": false, "[Channel2]": false};
D2.performanceMode = {"[Channel1]": "HOTCUE", "[Channel2]": "HOTCUE"};
D2.zoomLevel = {"[Channel1]": 2, "[Channel2]": 2};
D2.timeMode = {"[Channel1]": 0, "[Channel2]": 0};
D2.fxTouchMask = {"[Channel1]": 0, "[Channel2]": 0};
D2.beatgridEdit = {"[Channel1]": false, "[Channel2]": false};
D2.touchStripPressed = {"[Channel1]": false, "[Channel2]": false};
D2.loopTouched = {"[Channel1]": false, "[Channel2]": false};
D2.screenEncoderTouched = {"[Channel1]": 0, "[Channel2]": 0};
D2.faderTouched = {"[Channel1]": 0, "[Channel2]": 0};
D2.activeDeck = {"[Channel1]": 1, "[Channel2]": 2};
D2.fxUnit = {"[Channel1]": 1, "[Channel2]": 2};
D2.libraryFocus = 0;
D2.sentState = {};
D2.stemsSupported = false;
D2.phaseTimer = null;
D2.browseSnapshotTimer = null;
D2.positionConnections = [];
D2.positionLastSentAt = {"[Channel1]": 0, "[Channel2]": 0};
D2.browseSnapshotDelayMs = 45;
D2.phaseBeatStep = {"[Channel1]": 0, "[Channel2]": 0};
D2.phaseBeatActive = {"[Channel1]": false, "[Channel2]": false};
D2.cueHeld = {"[Channel1]": false, "[Channel2]": false};
D2.cuePreviewing = {"[Channel1]": false, "[Channel2]": false};

D2.deckNumber = function(group) {
    return group === "[Channel1]" ? 1 : 2;
};

D2.activeGroup = function(surfaceGroup) {
    return "[Channel" + D2.activeDeck[surfaceGroup] + "]";
};

D2.pulse = function(group, control) {
    engine.setValue(group, control, 1);
    engine.setValue(group, control, 0);
};

/* Main-cue position is stored in engine frames, while playposition is a
 * normalized 0..1 value.  Comparing the two avoids relying on the blinking
 * cue_indicator (which is also non-zero when a Pioneer cue is armed away
 * from the cue point). */
D2.isAtMainCue = function(group) {
    var position = Number(engine.getValue(group, "playposition"));
    var cueFrame = Number(engine.getValue(group, "cue_point"));
    var trackFrames = Number(engine.getValue(group, "track_samples"));
    if (!isFinite(position) || !isFinite(cueFrame) || !isFinite(trackFrames) ||
        trackFrames <= 0 || cueFrame < 0) return false;
    var cuePosition = cueFrame / trackFrames;
    /* A frame-accurate cue can still be reported with a small GUI/engine
     * delay.  Keep the tolerance below one video frame at normal tempos. */
    return Math.abs(position - cuePosition) <= 0.0025;
};

D2.isClockwise = function(value) {
    /* Bridge relative protocol: 65 clockwise, 63 counter-clockwise. Keep the
     * fallback for controllers that report small positive deltas, but never
     * classify the explicit 63 packet as clockwise. */
    return value === 0x41 || (value > 0 && value < 0x3F);
};

D2.sendSysexText = function(text) {
    var message = [0xF0, 0x7D];
    for (var i = 0; i < text.length; i++) {
        message.push(text.charCodeAt(i) & 0x7F);
    }
    message.push(0xF7);
    midi.sendSysexMsg(message, message.length);
};

D2.sendState = function(group, key, value, force) {
    var deck = D2.deckNumber(group);
    var cacheKey = deck + "|" + key;
    var text = String(value);
    if (force || D2.sentState[cacheKey] !== text) {
        D2.sentState[cacheKey] = text;
        D2.sendSysexText("D2|" + deck + "|" + key + "|" + text);
    }
};

D2.setDisplayView = function(group, view) {
    D2.sendState(group, "VIEW", view, true);
};

D2.setPerformanceMode = function(group, mode) {
    D2.performanceMode[group] = mode;
    D2.setDisplayView(group, mode);
    D2.refreshLEDs(group);
};

D2.effectUnitGroup = function(group) {
    return "[EffectRack1_EffectUnit" + D2.fxUnit[group] + "]";
};

D2.effectGroup = function(group, effect) {
    return "[EffectRack1_EffectUnit" + D2.fxUnit[group] +
           "_Effect" + effect + "]";
};

D2.sendDeckState = function(group) {
    var deck = D2.deckNumber(group);
    var activeGroup = D2.activeGroup(group);
    var bpm = Number(engine.getValue(activeGroup, "bpm") || 0);
    var position = Number(engine.getValue(activeGroup, "playposition") || 0);
    var playing = engine.getValue(activeGroup, "play") ? 1 : 0;
    var duration = Number(engine.getValue(activeGroup, "duration") || 0);
    var sampleRate = Number(engine.getValue(activeGroup, "track_samplerate") || 0);
    var rate = Number(engine.getValue(activeGroup, "rate_ratio") || 1);
    var beatDistance = Number(engine.getValue(activeGroup, "beat_distance") || 0);
    var beatPrevious = Number(engine.getValue(activeGroup, "beat_prev"));
    var beatNext = Number(engine.getValue(activeGroup, "beat_next"));
    var beatScale = duration > 0 && sampleRate > 0 ? duration * sampleRate : 0;
    var remaining = Math.max(0, duration * (1 - position));

    /* The live transport callbacks below already publish time-critical
     * changes.  This resilience snapshot must not repeat identical SysEx
     * packets forever: a busy controller output queue also delays input
     * callbacks on PortMidi. */
    D2.sendState(group, "DURATION", duration.toFixed(3));
    D2.sendState(group, "BPM", bpm.toFixed(1));
    D2.sendState(group, "RATE", rate.toFixed(5));
    D2.sendState(group, "POS", position.toFixed(6));
    D2.sendState(group, "PLAY", playing);
    D2.sendState(group, "REMAIN", remaining.toFixed(3));
    D2.sendState(group, "BEATDIST",
        Math.max(0, Math.min(1, beatDistance)).toFixed(6));
    D2.sendState(group, "BEATVALID",
        (beatPrevious >= 0 && beatNext > beatPrevious) ? "1" : "0");
    D2.sendState(group, "BEATPREV",
        beatScale > 0 && beatPrevious >= 0 ?
            (beatPrevious / beatScale).toFixed(8) : "-1");
    D2.sendState(group, "BEATNEXT",
        beatScale > 0 && beatNext > beatPrevious ?
            (beatNext / beatScale).toFixed(8) : "-1");
    D2.sendState(group, "ZOOM", D2.zoomLevel[group]);
    D2.sendState(group, "TIMEMODE", D2.timeMode[group]);
    D2.sendState(group, "LOOPSIZE",
        Number(engine.getValue(activeGroup, "beatloop_size") || 4).toFixed(5));
    D2.sendState(group, "CHANNEL", D2.activeDeck[group]);
    D2.sendState(group, "QUANTIZE", engine.getValue(activeGroup, "quantize") ? 1 : 0);
    D2.sendState(group, "KEYLOCK", engine.getValue(activeGroup, "keylock") ? 1 : 0);
    /* visual_key is Mixxx's already-transposed key.  Unlike the database key,
     * it changes immediately when Key Shift is adjusted in the main UI. */
    D2.sendState(group, "KEYVISUAL",
        Math.round(Number(engine.getValue(activeGroup, "visual_key") || 0)));

    var unit = D2.effectUnitGroup(group);
    D2.sendState(group, "FX1", Number(engine.getParameter(unit, "mix") || 0).toFixed(4));
    for (var effect = 1; effect <= 3; effect++) {
        var effectGroup = D2.effectGroup(group, effect);
        D2.sendState(group, "FX" + (effect + 1),
            Number(engine.getParameter(effectGroup, "meta") || 0).toFixed(4));
        D2.sendState(group, "FXEN" + effect,
            engine.getValue(effectGroup, "enabled") ? 1 : 0);
    }
    var allEnabled = engine.getValue(D2.effectGroup(group, 1), "enabled") &&
                     engine.getValue(D2.effectGroup(group, 2), "enabled") &&
                     engine.getValue(D2.effectGroup(group, 3), "enabled");
    D2.sendState(group, "FXEN4", allEnabled ? 1 : 0);

    /* Mixxx 2.5 has no stem Control Objects. The four performance strips are
     * therefore fully functional Sampler 1..4 controls instead of dead UI. */
    D2.sendState(group, "STEMCOUNT", 0);
    D2.refreshLEDs(group);
};

D2.padColor = function(group, pad) {
    var activeGroup = D2.activeGroup(group);
    var mode = D2.performanceMode[group] || "HOTCUE";
    var hotcueColors = [0x00A8FF, 0xFF7A00, 0x00D47B, 0xD060FF,
                        0xFF4050, 0xFFD000, 0x40D8FF, 0x70FF40];
    if (mode === "HOTCUE")
        return engine.getValue(activeGroup, "hotcue_" + pad + "_status") ?
            hotcueColors[pad - 1] : 0x080808;
    if (mode === "LOOP") {
        var loopBeats = [0.25, 0.5, 1, 2, 4, 8, 16, 32];
        var loopSize = Number(engine.getValue(activeGroup, "beatloop_size") || 4);
        var selected = Math.abs(loopSize - loopBeats[pad - 1]) < 0.001;
        return selected && engine.getValue(activeGroup, "loop_enabled") ?
            0x00FF60 : (selected ? 0x087030 : 0x042010);
    }
    if (mode === "FREEZE") {
        var rollBeats = [0.25, 0.5, 1, 2, 4, 8, 16, 32];
        return engine.getValue(activeGroup,
            "beatlooproll_" + rollBeats[pad - 1] + "_activate") ?
            0x4080FF : 0x102048;
    }
    if (mode === "BEATJUMP") return 0xFF7A00;
    if (mode === "SAMPLER")
        return engine.getValue("[Sampler" + pad + "]", "track_loaded") ? 0xFFB000 : 0x201000;
    return 0;
};

D2.refreshLEDs = function(group) {
    var activeGroup = D2.activeGroup(group);
    var modeNames = {HOTCUE: 1, LOOP: 2, FREEZE: 3, SAMPLER: 4, BEATJUMP: 5};
    var fxMask = 0;
    var onMask = 0;
    var assignMask = 0;
    for (var i = 1; i <= 4; i++) {
        if (i <= 3 && engine.getValue(D2.effectGroup(group, i), "enabled"))
            fxMask |= 1 << (i - 1);
        if (!engine.getValue("[Sampler" + i + "]", "mute"))
            onMask |= 1 << (i - 1);
        if (i <= 2 && engine.getValue(D2.effectUnitGroup(group),
                            "group_[Channel" + i + "]_enable"))
            assignMask |= 1 << (i - 1);
    }
    if ((fxMask & 0x07) === 0x07) fxMask |= 0x08;
    var fields = [
        engine.getValue(activeGroup, "play_indicator") ? 1 : 0,
        engine.getValue(activeGroup, "cue_indicator") ? 1 : 0,
        engine.getValue(activeGroup, "sync_enabled") ? 1 : 0,
        engine.getValue(activeGroup, "slip_enabled") ? 1 : 0,
        D2.shiftPressed[group] ? 1 : 0,
        engine.getValue(activeGroup, "loop_enabled") ? 1 : 0,
        D2.activeDeck[group],
        modeNames[D2.performanceMode[group]] || 1,
        D2.fxUnit[group],
        fxMask,
        onMask,
        assignMask
    ];
    for (i = 1; i <= 8; i++) {
        var color = D2.padColor(group, i).toString(16);
        while (color.length < 6) color = "0" + color;
        fields.push(color);
    }
    D2.sendState(group, "LEDPACK", fields.join(","));
};

D2.publishDeckMarkers = function(group) {
    var deck = D2.deckNumber(group);
    var activeGroup = D2.activeGroup(group);
    var duration = Number(engine.getValue(activeGroup, "duration") || 0);
    var sampleRate = Number(engine.getValue(activeGroup, "track_samplerate") || 0);
    D2.sendSysexText("D2|" + deck + "|SAMPLERATE|" + sampleRate.toFixed(1));
    for (var cue = 1; cue <= 8; cue++) {
        var frame = Number(engine.getValue(activeGroup, "hotcue_" + cue + "_position"));
        var normalized = frame >= 0 && duration > 0 && sampleRate > 0 ?
            frame / (duration * sampleRate) : -1;
        D2.sendSysexText("D2|" + deck + "|CUE" + cue + "|" + normalized.toFixed(6));
    }
};

D2.publishTrackLoad = function(group) {
    var duration = Number(engine.getValue(D2.activeGroup(group), "duration") || 0);
    if (duration > 1) {
        D2.sendState(group, "LOAD", duration.toFixed(3), true);
        D2.publishDeckMarkers(group);
    }
};

D2.trackLoaded = function(trackGroup, value) {
    if (!value) return;
    D2.phaseBeatStep[trackGroup] = 0;
    D2.phaseBeatActive[trackGroup] = false;
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) === trackGroup) {
            /* Browse encoder press no longer guesses that LOAD succeeded.
             * Keep Browse visible for an offline/missing library entry and
             * return to the player only after Mixxx confirms track_loaded. */
            D2.setDisplayView(surfaceGroup, "DECK");
            D2.publishTrackLoad(surfaceGroup);
        }
    });
};

D2.updateDisplays = function() {
    D2.sendDeckState("[Channel1]");
    D2.sendDeckState("[Channel2]");
};

/* Push only the single time-critical datum as Mixxx publishes it. This has
 * less latency than polling but avoids the four-message 30 Hz transport
 * flood that can starve the D2's shared MIDI/USB event loop. */
D2.positionChanged = function(trackGroup, value) {
    var now = Date.now();
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) !== trackGroup) return;
        /* Cap transport IPC at 30 Hz. The bridge interpolates the 60 Hz
         * render positions locally, so faster SysEx traffic adds contention
         * without improving the physical display. */
        if (now - D2.positionLastSentAt[surfaceGroup] < 33) return;
        D2.positionLastSentAt[surfaceGroup] = now;
        D2.sendSysexText("D2|" + D2.deckNumber(surfaceGroup) + "|POS|" +
            Number(value || 0).toFixed(7));
    });
};

D2.phaseMasterGroup = function(surfaceGroup) {
    if (engine.getValue("[Channel1]", "sync_leader")) return "[Channel1]";
    if (engine.getValue("[Channel2]", "sync_leader")) return "[Channel2]";
    /* With no explicit Sync Leader, compare each physical D2 against the
     * opposite player.  This keeps the meter useful during manual beatmatch. */
    return D2.activeGroup(surfaceGroup) === "[Channel1]" ?
        "[Channel2]" : "[Channel1]";
};

D2.phaseValue = function(group) {
    var phase = Number(engine.getValue(group, "beat_distance"));
    if (!isFinite(phase)) return 0;
    phase = phase - Math.floor(phase);
    return phase < 0 ? phase + 1 : phase;
};

D2.updatePhaseMeter = function() {
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        var activeGroup = D2.activeGroup(surfaceGroup);
        var masterGroup = D2.phaseMasterGroup(surfaceGroup);
        /* The D2 meter uses four fixed cells, one cell per beat. Continuous
         * beat_distance samples are deliberately not transmitted: the
         * renderer only consumes the discrete beat counters. */
        D2.sendState(surfaceGroup, "PHASE",
            "0.00000,0.00000," +
            D2.phaseBeatStep[masterGroup] + "," +
            D2.phaseBeatStep[activeGroup]);
    });
};

D2.beatActiveChanged = function(trackGroup, value) {
    var active = Number(value) > 0;
    if (active && !D2.phaseBeatActive[trackGroup]) {
        var direction = Number(value) >= 2 ? -1 : 1;
        D2.phaseBeatStep[trackGroup] =
            (D2.phaseBeatStep[trackGroup] + direction + 4) % 4;
        D2.updatePhaseMeter();
    }
    D2.phaseBeatActive[trackGroup] = active;
};

D2.hotcueChanged = function(trackGroup) {
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) === trackGroup) {
            D2.publishDeckMarkers(surfaceGroup);
            D2.refreshLEDs(surfaceGroup);
        }
    });
};

D2.visualKeyChanged = function(trackGroup, value) {
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) === trackGroup)
            D2.sendState(surfaceGroup, "KEYVISUAL", Math.round(Number(value) || 0), true);
    });
};

D2.liveStateChanged = function(trackGroup, control, value) {
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) !== trackGroup) return;
        if (control === "beatloop_size")
            D2.sendState(surfaceGroup, "LOOPSIZE", Number(value || 4).toFixed(5), true);
        else if (control === "quantize")
            D2.sendState(surfaceGroup, "QUANTIZE", value ? 1 : 0, true);
        else if (control === "keylock")
            D2.sendState(surfaceGroup, "KEYLOCK", value ? 1 : 0, true);
        else if (control === "rate_ratio")
            D2.sendState(surfaceGroup, "RATE", Number(value || 1).toFixed(5), true);
        else if (control === "bpm")
            D2.sendState(surfaceGroup, "BPM", Number(value || 0).toFixed(1), true);
        else if (control === "play_indicator")
            D2.sendState(surfaceGroup, "PLAY", value ? 1 : 0, true);
        D2.refreshLEDs(surfaceGroup);
    });
};

D2.publishBrowseSnapshot = function(force) {
    for (var row = 0; row < 9; row++) {
        var id = Math.round(engine.getValue("[Library]", "browse_track_id_" + row) || 0);
        /* Keep the compact nine-row model coherent, but do not flood the
         * MIDI queue with identical rows while the encoder is moving. */
        D2.sendState("[Channel1]", "BROWSE" + row, id, !!force);
        D2.sendState("[Channel2]", "BROWSE" + row, id, !!force);
    }
};

D2.queueBrowseSnapshot = function(delayMs, force) {
    if (D2.browseSnapshotTimer) {
        engine.stopTimer(D2.browseSnapshotTimer);
        D2.browseSnapshotTimer = null;
    }
    D2.browseSnapshotTimer = engine.beginTimer(
        delayMs === undefined ? D2.browseSnapshotDelayMs : delayMs,
        function() {
            D2.browseSnapshotTimer = null;
            D2.publishBrowseSnapshot(!!force);
        }, true);
};

D2.browseSelectionChanged = function(value) {
    /* Mixxx may emit several selected_track_id changes for one USB folder
     * update. A short trailing debounce leaves navigation native and fluid
     * while publishing only the settled, visible nine-row window. */
    D2.queueBrowseSnapshot(D2.browseSnapshotDelayMs, false);
};

D2.refreshBrowseModel = function() {
    D2.pulse("[Library]", "d2_browse_refresh");
    D2.queueBrowseSnapshot(90, true);
};

D2.refreshBrowseUntilSettled = function() {
    [120, 360, 800].forEach(function(delay) {
        engine.beginTimer(delay, function() {
            D2.pulse("[Library]", "d2_browse_refresh");
            D2.publishBrowseSnapshot(true);
        }, true);
    });
};

D2.browseTouch = function(channel, control, value, status, group) {
    if (value === 0x7F) {
        D2.libraryFocus = 0;
        engine.setValue("[Skin]", "show_maximized_library", 1);
        engine.setValue("[Library]", "focused_widget", 3);
        D2.sendState("[Channel1]", "BROWSEFOCUS", 0, true);
        D2.sendState("[Channel2]", "BROWSEFOCUS", 0, true);
        engine.beginTimer(60, D2.refreshBrowseModel, true);
    }
};

D2.browseEncoder = function(channel, control, value, status, group) {
    /* Use D2-specific direct controls instead of keyboard-focus-dependent
     * MoveVertical. This remains deterministic even when the touchscreen,
     * a dialog, or another desktop window owns QApplication focus. */
    /* The bridge emits the explicit relative pair 65 (clockwise) and
     * 63 (counter-clockwise).  Do not treat every value below 64 as
     * clockwise: 63 was the bug that made both directions move down. */
    var clockwise = value === 0x41;
    var controlName = D2.libraryFocus ?
        (clockwise ? "d2_sidebar_down" : "d2_sidebar_up") :
        (clockwise ? "d2_track_down" : "d2_track_up");
    var steps = D2.shiftPressed[group] ? 8 : 1;
    for (var i = 0; i < steps; i++) {
        engine.setValue("[Library]", controlName, 1);
        engine.setValue("[Library]", controlName, 0);
    }
    /* selected_track_id also queues this update; do not publish a second
     * nine-row snapshot for the same encoder detent. */
};

D2.loadSelectedTrack = function(channel, control, value, status, group) {
    if (!value) return;
    if (D2.libraryFocus) {
        /* In the tree the encoder press activates/expands the selected item.
         * Leaf items move Qt focus to the track table; branch items stay in
         * the tree so another press/turn can continue navigating. */
        var isLeaf = engine.getValue("[Library]", "d2_sidebar_is_leaf") > 0;
        D2.pulse("[Library]", "d2_sidebar_activate");
        D2.libraryFocus = isLeaf ? 0 : 1;
        D2.sendState("[Channel1]", "BROWSEFOCUS", D2.libraryFocus, true);
        D2.sendState("[Channel2]", "BROWSEFOCUS", D2.libraryFocus, true);
        D2.refreshBrowseUntilSettled();
        return;
    }
    /* LoadSelectedTrack is queued onto Mixxx's library/UI thread. Sending a
     * same-tick 0 release can cancel it before that thread consumes the
     * trigger, which made D2 LOAD appear dead while touchscreen LOAD worked. */
    engine.setValue(D2.activeGroup(group), "LoadSelectedTrack", 1);
    engine.setValue("[Skin]", "show_maximized_library", 0);
    D2.setDisplayView(group, "DECK");
};

D2.backButton = function(channel, control, value, status, group) {
    if (!value) return;
    /* Select the exact widget. Tab traversal is skin-dependent and could land
     * on Search instead of the Sidebar in compact two-deck layouts. */
    D2.libraryFocus = D2.libraryFocus ? 0 : 1;
    engine.setValue("[Library]", "focused_widget", D2.libraryFocus ? 2 : 3);
    D2.sendState("[Channel1]", "BROWSEFOCUS", D2.libraryFocus, true);
    D2.sendState("[Channel2]", "BROWSEFOCUS", D2.libraryFocus, true);
    engine.beginTimer(30, D2.refreshBrowseModel, true);
};

D2.init = function(id, debugging) {
    D2.setDisplayView("[Channel1]", "DECK");
    D2.setDisplayView("[Channel2]", "DECK");
    D2.trackLoadConnections = [];
    D2.hotcueConnections = [];
    D2.keyConnections = [];
    D2.beatConnections = [];
    D2.liveStateConnections = [];
    D2.positionConnections = [];
    for (var deck = 1; deck <= 2; deck++) {
        (function(trackGroup) {
            D2.trackLoadConnections.push(engine.makeConnection(
                trackGroup, "track_loaded", function(value) {
                    D2.trackLoaded(trackGroup, value);
                }));
            for (var cue = 1; cue <= 8; cue++) {
                D2.hotcueConnections.push(engine.makeConnection(
                    trackGroup, "hotcue_" + cue + "_position", function() {
                        D2.hotcueChanged(trackGroup);
                    }));
            }
            D2.keyConnections.push(engine.makeConnection(
                trackGroup, "visual_key", function(value) {
                    D2.visualKeyChanged(trackGroup, value);
                }));
            D2.beatConnections.push(engine.makeConnection(
                trackGroup, "beat_active", function(value) {
                    D2.beatActiveChanged(trackGroup, value);
                }));
            D2.positionConnections.push(engine.makeConnection(
                trackGroup, "playposition", function(value) {
                    D2.positionChanged(trackGroup, value);
                }));
            ["play_indicator", "cue_indicator", "sync_enabled", "slip_enabled",
             "loop_enabled", "beatloop_size", "quantize", "keylock",
             "rate_ratio", "bpm"].forEach(function(control) {
                D2.liveStateConnections.push(engine.makeConnection(
                    trackGroup, control, function() {
                        D2.liveStateChanged(trackGroup, control,
                            engine.getValue(trackGroup, control));
                    }));
            });
        })("[Channel" + deck + "]");
    }
    D2.browseConnection = engine.makeConnection(
        "[Library]", "selected_track_id", D2.browseSelectionChanged);
    D2.publishTrackLoad("[Channel1]");
    D2.publishTrackLoad("[Channel2]");
    D2.updateDisplays();
    /* One eager snapshot at startup gives both displays usable data. Every
     * subsequent selection change is debounced by browseSelectionChanged(). */
    D2.publishBrowseSnapshot(true);
    /* Full state is event-driven; this is only a low-rate resilience snapshot.
     * Position is sent from the engine's actual playposition callback above. */
    D2.displayTimer = engine.beginTimer(500, D2.updateDisplays);
    D2.updatePhaseMeter();
    /* Beat connections publish changes immediately. This low-rate timer is
     * only a recovery snapshot and normally emits nothing due to caching. */
    D2.phaseTimer = engine.beginTimer(500, D2.updatePhaseMeter);
};

D2.shutdown = function() {
    if (D2.trackLoadConnections) {
        for (var i = 0; i < D2.trackLoadConnections.length; i++)
            D2.trackLoadConnections[i].disconnect();
    }
    if (D2.browseConnection) D2.browseConnection.disconnect();
    if (D2.hotcueConnections) {
        for (var cueConnection = 0; cueConnection < D2.hotcueConnections.length;
             cueConnection++)
            D2.hotcueConnections[cueConnection].disconnect();
    }
    if (D2.keyConnections) {
        for (var keyConnection = 0; keyConnection < D2.keyConnections.length;
             keyConnection++)
            D2.keyConnections[keyConnection].disconnect();
    }
    if (D2.beatConnections) {
        for (var beatConnection = 0; beatConnection < D2.beatConnections.length;
             beatConnection++)
            D2.beatConnections[beatConnection].disconnect();
    }
    if (D2.positionConnections) {
        for (var positionConnection = 0;
             positionConnection < D2.positionConnections.length;
             positionConnection++)
            D2.positionConnections[positionConnection].disconnect();
    }
    if (D2.liveStateConnections) {
        for (var stateConnection = 0;
             stateConnection < D2.liveStateConnections.length; stateConnection++)
            D2.liveStateConnections[stateConnection].disconnect();
    }
    if (D2.displayTimer) engine.stopTimer(D2.displayTimer);
    if (D2.phaseTimer) engine.stopTimer(D2.phaseTimer);
    if (D2.browseSnapshotTimer) engine.stopTimer(D2.browseSnapshotTimer);
    D2.trackLoadConnections = null;
    D2.hotcueConnections = null;
    D2.keyConnections = null;
    D2.beatConnections = null;
    D2.positionConnections = null;
    D2.liveStateConnections = null;
    D2.browseConnection = null;
    D2.displayTimer = null;
    D2.phaseTimer = null;
    D2.browseSnapshotTimer = null;
};

D2.shiftButton = function(channel, control, value, status, group) {
    D2.shiftPressed[group] = value === 0x7F;
    D2.refreshLEDs(group);
};

D2.syncButton = function(channel, control, value, status, group) {
    if (!value) return;
    var activeGroup = D2.activeGroup(group);
    if (D2.shiftPressed[group])
        engine.setValue(activeGroup, "sync_enabled", !engine.getValue(activeGroup, "sync_enabled"));
    else
        D2.pulse(activeGroup, "beatsync");
    D2.refreshLEDs(group);
};

D2.deckButton = function(channel, control, value, status, group) {
    if (!value) return;
    /* This custom Mixxx build instantiates two player decks. Keep the native
     * DECK button deterministic: it always dismisses overlays and restores
     * the corresponding D1/D2 player instead of addressing absent C/D decks. */
    D2.setDisplayView(group, "DECK");
    D2.sendDeckState(group);
};

D2.fxAssignButton = function(channel, control, value, status, group) {
    if (!value) return;
    var targetDeck = control - 0x24 + 1;
    /* This image instantiates two player decks.  The physical C/D assignment
     * buttons remain harmless instead of polling non-existent Channel3/4 COs. */
    if (targetDeck < 1 || targetDeck > 2) return;
    var targetGroup = "[Channel" + targetDeck + "]";
    var unitGroup = D2.effectUnitGroup(group);
    var key = "group_" + targetGroup + "_enable";
    engine.setValue(unitGroup, key, !engine.getValue(unitGroup, key));
    D2.refreshLEDs(group);
};

D2.fxSelectButton = function(channel, control, value, status, group) {
    if (!value) return;
    D2.fxUnit[group] = D2.fxUnit[group] === 1 ? 2 : 1;
    D2.fxTouchMask[group] = 0;
    D2.sendState(group, "FXTOUCH", 0, true);
    D2.sendDeckState(group);
};

D2.playButton = function(channel, control, value, status, group) {
    if (!value) return;
    var activeGroup = D2.activeGroup(group);
    /* During a main-cue preview Mixxx reports play=1. Its native cue engine
     * explicitly treats a following play=0 request as a latch command: it
     * clears preview state and keeps playback running. */
    if (D2.cuePreviewing[group]) {
        engine.setValue(activeGroup, "play", 0);
        D2.cueHeld[group] = false;
        D2.cuePreviewing[group] = false;
    } else {
        /* PLAY is a true pause/play toggle outside CUE preview. */
        var playing = engine.getValue(activeGroup, "play") ? 1 : 0;
        engine.setValue(activeGroup, "play", playing ? 0 : 1);
    }
    D2.sendDeckState(group);
};

D2.cueButton = function(channel, control, value, status, group) {
    var activeGroup = D2.activeGroup(group);
    if (D2.shiftPressed[group]) {
        if (value) D2.pulse(activeGroup, "cue_gotoandstop");
        D2.sendDeckState(group);
        return;
    }

    if (value) {
        var playing = engine.getValue(activeGroup, "play") ? 1 : 0;
        if (playing) {
            /* Playing: stop immediately and return to the stored main cue. */
            D2.pulse(activeGroup, "cue_gotoandstop");
            D2.cueHeld[group] = false;
            D2.cuePreviewing[group] = false;
        } else if (D2.isAtMainCue(activeGroup)) {
            /* Stopped at cue: start momentary preview; release returns to cue. */
            D2.cueHeld[group] = true;
            D2.cuePreviewing[group] = true;
            engine.setValue(activeGroup, "cue_preview", 1);
        } else {
            /* Stopped away from cue: Pioneer stores this position as the new
             * main cue and immediately previews from it while CUE is held. */
            D2.pulse(activeGroup, "cue_set");
            D2.cueHeld[group] = true;
            D2.cuePreviewing[group] = true;
            engine.setValue(activeGroup, "cue_preview", 1);
        }
    } else if (D2.cuePreviewing[group]) {
        /* The dedicated CUE CC transmits release reliably. Let Mixxx's native
         * preview release stop playback and seek exactly back to main cue. */
        engine.setValue(activeGroup, "cue_preview", 0);
        D2.cueHeld[group] = false;
        D2.cuePreviewing[group] = false;
    }
    D2.sendDeckState(group);
};

D2.hotcueButton = function(channel, control, value, status, group) {
    if (value) D2.setPerformanceMode(group, "HOTCUE");
};

D2.loopButton = function(channel, control, value, status, group) {
    if (value) D2.setPerformanceMode(group, "LOOP");
};

D2.freezeButton = function(channel, control, value, status, group) {
    if (value) D2.setPerformanceMode(group, "FREEZE");
};

D2.remixButton = function(channel, control, value, status, group) {
    if (value) D2.setPerformanceMode(group, "SAMPLER");
};

D2.padButton = function(channel, control, value, status, group) {
    var pad = control - 0x4B;
    if (pad < 1 || pad > 8) return;
    var activeGroup = D2.activeGroup(group);
    var mode = D2.performanceMode[group] || "HOTCUE";
    var loopBeats = [0.25, 0.5, 1, 2, 4, 8, 16, 32];
    var jumpBeats = [1, 1, 4, 4, 8, 8, 16, 16];
    var jumpDirection = ["backward", "forward", "backward", "forward",
                         "backward", "forward", "backward", "forward"];
    var controlName;
    if (mode === "HOTCUE") {
        if (D2.shiftPressed[group]) {
            if (value) D2.pulse(activeGroup, "hotcue_" + pad + "_clear");
        } else {
            /* Preserve CDJ-style hold/release preview instead of collapsing the
             * pad to a zero-length pulse. */
            engine.setValue(activeGroup, "hotcue_" + pad + "_activate", value ? 1 : 0);
        }
        if (value) D2.publishDeckMarkers(group);
        D2.refreshLEDs(group);
        return;
    } else if (mode === "FREEZE") {
        /* Beatloop-roll is momentary: it must remain active for exactly as long
         * as the physical pad is held. */
        controlName = "beatlooproll_" + loopBeats[pad - 1] + "_activate";
        engine.setValue(activeGroup, controlName, value ? 1 : 0);
        D2.refreshLEDs(group);
        return;
    }
    if (!value) return;
    if (mode === "LOOP") {
        controlName = "beatloop_" + loopBeats[pad - 1] + "_activate";
        D2.pulse(activeGroup, controlName);
    } else if (mode === "BEATJUMP") {
        controlName = "beatjump_" + jumpBeats[pad - 1] + "_" + jumpDirection[pad - 1];
        D2.pulse(activeGroup, controlName);
    } else if (mode === "SAMPLER") {
        var samplerGroup = "[Sampler" + pad + "]";
        D2.pulse(samplerGroup, D2.shiftPressed[group] ? "cue_gotoandstop" : "start_play");
    }
    D2.refreshLEDs(group);
};

D2.fluxButton = function(channel, control, value, status, group) {
    if (!value) return;
    var activeGroup = D2.activeGroup(group);
    engine.setValue(activeGroup, "slip_enabled", !engine.getValue(activeGroup, "slip_enabled"));
    D2.refreshLEDs(group);
};

D2.leftScreenButton = function(channel, control, value, status, group) {
    if (!value) return;
    var activeGroup = D2.activeGroup(group);
    var button = control - 0x31;
    if (button === 0) {
        D2.zoomLevel[group] = D2.zoomLevel[group] === 2 ? 4 :
                              (D2.zoomLevel[group] === 4 ? 8 : 2);
        D2.sendState(group, "ZOOM", D2.zoomLevel[group], true);
    } else if (button === 1) {
        engine.setValue(activeGroup, "keylock", !engine.getValue(activeGroup, "keylock"));
    } else if (button === 2) {
        D2.pulse(activeGroup, "sync_key");
    } else if (button === 3) {
        D2.timeMode[group] = D2.timeMode[group] ? 0 : 1;
        D2.sendState(group, "TIMEMODE", D2.timeMode[group], true);
    }
    D2.refreshLEDs(group);
};

D2.rightScreenButton = function(channel, control, value, status, group) {
    if (!value) return;
    var button = control - 0x35;
    if (button === 0) D2.setPerformanceMode(group, "HOTCUE");
    else if (button === 1) D2.setPerformanceMode(group, "LOOP");
    else if (button === 2) D2.setPerformanceMode(group, "BEATJUMP");
    else if (button === 3) {
        var activeGroup = D2.activeGroup(group);
        engine.setValue(activeGroup, "quantize", !engine.getValue(activeGroup, "quantize"));
    }
    D2.refreshLEDs(group);
};

D2.fxKnob = function(channel, control, value, status, group) {
    var slot = control - 0x24;
    var parameter = Math.max(0, Math.min(1, value / 127));
    if (slot === 0) engine.setParameter(D2.effectUnitGroup(group), "mix", parameter);
    else if (slot >= 1 && slot <= 3)
        engine.setParameter(D2.effectGroup(group, slot), "meta", parameter);
    D2.sendState(group, "FX" + (slot + 1), parameter.toFixed(4), true);
};

D2.fxButton = function(channel, control, value, status, group) {
    if (!value) return;
    var slot = control - 0x28;
    if (slot >= 0 && slot <= 2) {
        var effectGroup = D2.effectGroup(group, slot + 1);
        engine.setValue(effectGroup, "enabled", !engine.getValue(effectGroup, "enabled"));
    } else if (slot === 3) {
        var enableAll = !(engine.getValue(D2.effectGroup(group, 1), "enabled") &&
                          engine.getValue(D2.effectGroup(group, 2), "enabled") &&
                          engine.getValue(D2.effectGroup(group, 3), "enabled"));
    for (var effect = 1; effect <= 3; effect++)
            engine.setValue(D2.effectGroup(group, effect), "enabled", enableAll);
    }
    /* An FX push must never leave the capacitive overlay latched on screen. */
    D2.fxTouchMask[group] = 0;
    D2.sendState(group, "FXTOUCH", 0, true);
    D2.sendDeckState(group);
};

D2.fxTouch = function(channel, control, value, status, group) {
    var slot = control - 0x2C;
    var mask = D2.fxTouchMask[group] || 0;
    if (value) mask |= 1 << slot;
    else mask &= ~(1 << slot);
    D2.fxTouchMask[group] = mask;
    D2.sendState(group, "FXTOUCH", mask, true);
};

D2.stemFader = function(channel, control, value, status, group) {
    var stem = control - 0x20 + 1;
    if (stem < 1 || stem > 4) return;
    engine.setParameter("[Sampler" + stem + "]", "volume",
                        Math.max(0, Math.min(1, value / 127)));
};

D2.stemMute = function(channel, control, value, status, group) {
    if (!value) return;
    var stem = control - 0x44 + 1;
    if (stem < 1 || stem > 4) return;
    var samplerGroup = "[Sampler" + stem + "]";
    engine.setValue(samplerGroup, "mute", !engine.getValue(samplerGroup, "mute"));
    D2.refreshLEDs(group);
};

D2.faderTouch = function(channel, control, value, status, group) {
    var fader = control - 0x48;
    if (fader < 0 || fader > 3) return;
    var mask = D2.faderTouched[group] || 0;
    if (value) mask |= 1 << fader;
    else mask &= ~(1 << fader);
    D2.faderTouched[group] = mask;
    D2.sendState(group, "FADERTOUCH", mask, true);
};

D2.touchStripTouch = function(channel, control, value, status, group) {
    D2.touchStripPressed[group] = value !== 0;
    if (!value && !D2.shiftPressed[group]) engine.setValue(D2.activeGroup(group), "jog", 0);
};

D2.touchStrip = function(channel, control, value, status, group) {
    var normalized = Math.max(0, Math.min(1, value / 127));
    var activeGroup = D2.activeGroup(group);
    if (D2.shiftPressed[group]) engine.setValue(activeGroup, "playposition", normalized);
    else if (D2.touchStripPressed[group]) engine.setValue(activeGroup, "jog", (normalized - 0.5) * 6.0);
};

D2.captureButton = function(channel, control, value, status, group) {
    if (!value) return;
    engine.setValue("[Library]", "AutoDjAddBottom", 1);
    engine.setValue("[Library]", "AutoDjAddBottom", 0);
};

D2.editButton = function(channel, control, value, status, group) {
    if (!value) return;
    D2.beatgridEdit[group] = !D2.beatgridEdit[group];
    D2.setDisplayView(group, "DECK");
};

D2.screenEncoder = function(channel, control, value, status, group) {
    var encoder = control - 0x10;
    if (encoder < 0 || encoder > 3) return;
    var clockwise = D2.isClockwise(value);
    if (!D2.beatgridEdit[group]) {
        var samplerGroup = "[Sampler" + (encoder + 1) + "]";
        D2.pulse(samplerGroup, clockwise ? "pregain_up_small" : "pregain_down_small");
        return;
    }
    var activeGroup = D2.activeGroup(group);
    var controlName = null;
    if (encoder === 0) controlName = clockwise ? "beats_translate_later" : "beats_translate_earlier";
    if (encoder === 1) controlName = clockwise ? "beats_adjust_faster" : "beats_adjust_slower";
    if (encoder === 2) controlName = clockwise ? "beatjump_1_forward" : "beatjump_1_backward";
    if (encoder === 3) controlName = clockwise ? "loop_double" : "loop_halve";
    if (controlName) {
        D2.pulse(activeGroup, controlName);
    }
};

D2.screenEncoderTouch = function(channel, control, value, status, group) {
    var encoder = control - 0x39;
    if (encoder < 0 || encoder > 3) return;
    var mask = D2.screenEncoderTouched[group] || 0;
    if (value) mask |= 1 << encoder;
    else mask &= ~(1 << encoder);
    D2.screenEncoderTouched[group] = mask;
    D2.sendState(group, "ENCTOUCH", mask, true);
};

D2.loopEncoder = function(channel, control, value, status, group) {
    var activeGroup = D2.activeGroup(group);
    D2.pulse(activeGroup, D2.isClockwise(value) ? "loop_double" : "loop_halve");
    D2.sendState(group, "LOOPSIZE", Number(engine.getValue(activeGroup, "beatloop_size") || 4), true);
};

D2.loopPress = function(channel, control, value, status, group) {
    if (!value) return;
    D2.pulse(D2.activeGroup(group), "beatloop_activate");
    D2.refreshLEDs(group);
};

D2.loopTouch = function(channel, control, value, status, group) {
    D2.loopTouched[group] = value !== 0;
    D2.sendState(group, "LOOPTOUCH", D2.loopTouched[group] ? 1 : 0, true);
};
