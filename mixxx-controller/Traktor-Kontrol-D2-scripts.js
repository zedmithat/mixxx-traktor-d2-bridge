var D2 = function () {};

D2.shiftPressed = {"[Channel1]": false, "[Channel2]": false};
D2.performanceMode = {"[Channel1]": "HOTCUE", "[Channel2]": "HOTCUE"};
D2.zoomLevel = {"[Channel1]": 2, "[Channel2]": 2};
D2.timeMode = {"[Channel1]": 0, "[Channel2]": 0};
D2.fxTouchMask = {"[Channel1]": 0, "[Channel2]": 0};
D2.fxSelectionToken = {"[Channel1]": 0, "[Channel2]": 0};
D2.fxSettingsVisible = {"[Channel1]": false, "[Channel2]": false};
D2.beatgridEdit = {"[Channel1]": false, "[Channel2]": false};
D2.touchStripPressed = {"[Channel1]": false, "[Channel2]": false};
D2.touchStripState = {"[Channel1]": null, "[Channel2]": null};
D2.touchStripBendScale = 0.5;
D2.skinPage = {player: 0, browse: 1};
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
D2.browseSortTimer = null;
D2.markerRefreshTimer = {"[Channel1]": null, "[Channel2]": null};
D2.markerRefreshToken = {"[Channel1]": 0, "[Channel2]": 0};
D2.markerRefreshForce = {"[Channel1]": false, "[Channel2]": false};
D2.smartMenu = {"[Channel1]": false, "[Channel2]": false};
D2.smartIndex = {"[Channel1]": 0, "[Channel2]": 0};
D2.smartListCount = 6;
D2.positionConnections = [];
D2.positionLastSentAt = {"[Channel1]": 0, "[Channel2]": 0};
D2.browseSnapshotDelayMs = 45;
D2.browseSortColumn = {title: 2, bpm: 15, key: 20};
D2.phaseBeatStep = {"[Channel1]": 0, "[Channel2]": 0};
D2.phaseBeatActive = {"[Channel1]": false, "[Channel2]": false};
D2.padHeldMask = {"[Channel1]": 0, "[Channel2]": 0};
D2.cueHeld = {"[Channel1]": false, "[Channel2]": false};
D2.cuePreviewing = {"[Channel1]": false, "[Channel2]": false};
D2.pendingTrackId = {"[Channel1]": 0, "[Channel2]": 0};
D2.identityPublished = {"[Channel1]": false, "[Channel2]": false};
D2.defaultHotcueColors = [0x00A8FF, 0xFF7A00, 0x00D47B, 0xD060FF,
                          0xFF4050, 0xFFD000, 0x40D8FF, 0x70FF40];

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

/* Match Mixxx's on-screen BPM display. visual_bpm includes the live rate and
 * may briefly be zero while a track is being attached, so retain bpm as the
 * authoritative load-time fallback. */
D2.displayBpm = function(group) {
    var visual = Number(engine.getValue(group, "visual_bpm") || 0);
    if (isFinite(visual) && visual > 0) return visual;
    var analysed = Number(engine.getValue(group, "bpm") || 0);
    return isFinite(analysed) && analysed > 0 ? analysed : 0;
};

/* Effect names are QString snapshots from Mixxx's authoritative loaded
 * EffectManifest. Keep the wire payload 7-bit clean and bounded so it stays
 * inside one complete SysEx packet. U: is an explicit encoding version and
 * also lets an empty slot travel as the non-empty value "U:". */
D2.fxNameWireValue = function(value) {
    var source = String(value || "");
    var safe = "";
    var utf8Bytes = 0;
    for (var i = 0; i < source.length;) {
        var first = source.charCodeAt(i++);
        var piece;
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (i < source.length) {
                var second = source.charCodeAt(i);
                if (second >= 0xDC00 && second <= 0xDFFF) {
                    piece = String.fromCharCode(first, second);
                    i++;
                } else {
                    piece = "\uFFFD";
                }
            } else {
                piece = "\uFFFD";
            }
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            piece = "\uFFFD";
        } else if (first < 0x20 || first === 0x7F) {
            piece = " ";
        } else {
            piece = String.fromCharCode(first);
        }
        var encodedPiece = encodeURIComponent(piece);
        var pieceBytes = encodedPiece.replace(/%[0-9A-Fa-f]{2}/g, "x").length;
        if (utf8Bytes + pieceBytes > 63) break;
        safe += piece;
        utf8Bytes += pieceBytes;
    }
    return "U:" + encodeURIComponent(safe);
};

D2.sendFxNameState = function(group, effect, force) {
    if (typeof engine.getEffectName !== "function") return;
    var effectGroup = D2.effectGroup(group, effect);
    D2.sendState(group, "FXNAME" + effect,
        D2.fxNameWireValue(engine.getEffectName(effectGroup)), !!force);
};

D2.sendFxIdentity = function(group, effect, selection) {
    var selected = Math.max(0, Math.round(Number(selection || 0)));
    var cacheKey = D2.deckNumber(group) + "|FXSEL" + effect;
    var selectionChanged = D2.sentState[cacheKey] !== String(selected);
    D2.sendState(group, "FXSEL" + effect, selected);
    /* FXSEL clears the bridge-side old name. Force the following name when
     * the numeric index changed even if two effects share a display name. */
    D2.sendFxNameState(group, effect, selectionChanged);
};

/* Track::getId() can temporarily be invalid for restored or file-browser
 * loads even though Mixxx already knows the exact file.  Transmit that exact
 * UTF-8 location as percent-encoded, 7-bit-safe SysEx chunks.  The bridge
 * resolves the library ID with track_locations.location = ?, never by BPM or
 * duration. */
D2.sendTrackLocation = function(group, location) {
    var encoded = encodeURIComponent(String(location || ""));
    if (!encoded.length) return false;
    D2.sendState(group, "LOCBEGIN", encoded.length, true);
    for (var offset = 0; offset < encoded.length; offset += 120) {
        D2.sendState(group, "LOCCHUNK", encoded.slice(offset, offset + 120), true);
    }
    D2.sendState(group, "LOCEND", 1, true);
    return true;
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
    var unit = D2.deckNumber(group);
    D2.fxUnit[group] = unit;
    return "[EffectRack1_EffectUnit" + unit + "]";
};

D2.effectGroup = function(group, effect) {
    var unit = D2.deckNumber(group);
    D2.fxUnit[group] = unit;
    return "[EffectRack1_EffectUnit" + unit +
           "_Effect" + effect + "]";
};

D2.forEachSurfaceUsingFxUnit = function(unit, callback) {
    ["[Channel1]", "[Channel2]"].forEach(function(surface) {
        if (D2.deckNumber(surface) === unit) callback(surface);
    });
};

D2.publishFxOverlay = function(group, force) {
    var mask = D2.fxSettingsVisible[group] ? 0x0F :
        (D2.fxTouchMask[group] || 0);
    D2.sendState(group, "FXTOUCH", mask, force);
};

D2.sendFxSlotState = function(group, effect) {
    if (effect < 1 || effect > 3) return;
    var effectGroup = D2.effectGroup(group, effect);
    D2.sendState(group, "FX" + (effect + 1),
        Number(engine.getParameter(effectGroup, "meta") || 0).toFixed(4));
    D2.sendState(group, "FXEN" + effect,
        engine.getValue(effectGroup, "enabled") ? 1 : 0);
    D2.sendFxIdentity(group, effect,
        engine.getValue(effectGroup, "loaded_effect"));
};

D2.sendFxUnitState = function(group) {
    var unit = D2.effectUnitGroup(group);
    D2.sendState(group, "FX1",
        Number(engine.getParameter(unit, "mix") || 0).toFixed(4));
    for (var effect = 1; effect <= 3; effect++)
        D2.sendFxSlotState(group, effect);
};

D2.showFxSelection = function(group, effect) {
    var mask = 1 << effect; /* bit 0 is MIX, bits 1..3 are effect slots */
    var token = (D2.fxSelectionToken[group] || 0) + 1;
    D2.fxSelectionToken[group] = token;
    D2.fxSettingsVisible[group] = false;
    D2.fxTouchMask[group] = mask;
    D2.publishFxOverlay(group, true);
    engine.beginTimer(900, function() {
        if (D2.fxSelectionToken[group] !== token) return;
        D2.fxTouchMask[group] = 0;
        D2.publishFxOverlay(group, true);
    }, true);
};

D2.sendDeckState = function(group, forceTrackDerived) {
    var deck = D2.deckNumber(group);
    var activeGroup = D2.activeGroup(group);
    var bpm = D2.displayBpm(activeGroup);
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
    var forceTrack = !!forceTrackDerived;
    D2.sendState(group, "DURATION", duration.toFixed(3), forceTrack);
    D2.sendState(group, "BPM", bpm.toFixed(2), forceTrack);
    D2.sendState(group, "RATE", rate.toFixed(5), forceTrack);
    D2.sendState(group, "POS", position.toFixed(6), forceTrack);
    D2.sendState(group, "PLAY", playing, forceTrack);
    D2.sendState(group, "REMAIN", remaining.toFixed(3), forceTrack);
    D2.sendState(group, "BEATDIST",
        Math.max(0, Math.min(1, beatDistance)).toFixed(6), forceTrack);
    D2.sendState(group, "BEATVALID",
        (beatPrevious >= 0 && beatNext > beatPrevious) ? "1" : "0",
        forceTrack);
    D2.sendState(group, "BEATPREV",
        beatScale > 0 && beatPrevious >= 0 ?
            (beatPrevious / beatScale).toFixed(8) : "-1", forceTrack);
    D2.sendState(group, "BEATNEXT",
        beatScale > 0 && beatNext > beatPrevious ?
            (beatNext / beatScale).toFixed(8) : "-1", forceTrack);
    D2.sendState(group, "ZOOM", D2.zoomLevel[group]);
    D2.sendState(group, "TIMEMODE", D2.timeMode[group]);
    D2.sendState(group, "LOOPSIZE",
        Number(engine.getValue(activeGroup, "beatloop_size") || 4).toFixed(5));
    D2.sendState(group, "QUANTIZE", engine.getValue(activeGroup, "quantize") ? 1 : 0);
    D2.sendState(group, "KEYLOCK", engine.getValue(activeGroup, "keylock") ? 1 : 0);
    /* visual_key is Mixxx's already-transposed key.  Unlike the database key,
     * it changes immediately when Key Shift is adjusted in the main UI. */
    D2.sendState(group, "KEYVISUAL",
        Math.round(Number(engine.getValue(activeGroup, "visual_key") || 0)),
        forceTrack);
    D2.sendState(group, "GRIDEDIT", D2.beatgridEdit[group] ? 1 : 0);

    D2.sendFxUnitState(group);
    /* Mixxx 2.5 has no stem Control Objects. The four performance strips are
     * therefore fully functional Sampler 1..4 controls instead of dead UI. */
    D2.sendState(group, "STEMCOUNT", 0);
    D2.refreshLEDs(group);
};

D2.padColor = function(group, pad) {
    var activeGroup = D2.activeGroup(group);
    var mode = D2.performanceMode[group] || "HOTCUE";
    if (mode === "HOTCUE")
        return engine.getValue(activeGroup, "hotcue_" + pad + "_status") ?
            D2.hotcueColor(activeGroup, pad) : 0x080808;
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
    if (mode === "BEATJUMP")
        return (D2.padHeldMask[group] & (1 << (pad - 1))) ?
            0xFFB000 : 0x402000;
    if (mode === "SAMPLER")
        return engine.getValue("[Sampler" + pad + "]", "track_loaded") ? 0xFFB000 : 0x201000;
    return 0;
};

D2.hotcueColor = function(group, hotcue) {
    var color = Math.round(Number(
        engine.getValue(group, "hotcue_" + hotcue + "_color")));
    if (isFinite(color) && color > 0 && color <= 0xFFFFFF)
        return color >>> 0;
    return D2.defaultHotcueColors[hotcue - 1] || 0x00A8FF;
};

D2.refreshLEDs = function(group) {
    var activeGroup = D2.activeGroup(group);
    var ownDeck = D2.deckNumber(group);
    var unitGroup = D2.effectUnitGroup(group);
    var modeNames = {HOTCUE: 1, LOOP: 2, FREEZE: 3, SAMPLER: 4, BEATJUMP: 5};
    var fxMask = 0;
    var onMask = 0;
    var assignMask = 0;
    if (engine.getValue(unitGroup,
            "group_[Channel" + ownDeck + "]_enable"))
        fxMask |= 0x01;
    for (var i = 1; i <= 4; i++) {
        if (i <= 3 && engine.getValue(D2.effectGroup(group, i), "enabled"))
            fxMask |= 1 << i;
        if (!engine.getValue("[Sampler" + i + "]", "mute"))
            onMask |= 1 << (i - 1);
        if (i <= 2 && engine.getValue(unitGroup,
                            "group_[Channel" + i + "]_enable"))
            assignMask |= 1 << (i - 1);
    }
    var fields = [
        engine.getValue(activeGroup, "play_indicator") ? 1 : 0,
        engine.getValue(activeGroup, "cue_indicator") ? 1 : 0,
        engine.getValue(activeGroup, "sync_enabled") ? 1 : 0,
        engine.getValue(activeGroup, "slip_enabled") ? 1 : 0,
        D2.shiftPressed[group] ? 1 : 0,
        engine.getValue(activeGroup, "loop_enabled") ? 1 : 0,
        D2.activeDeck[group],
        modeNames[D2.performanceMode[group]] || 1,
        ownDeck,
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

D2.publishDeckMarkers = function(group, force) {
    var activeGroup = D2.activeGroup(group);
    var duration = Number(engine.getValue(activeGroup, "duration") || 0);
    var sampleRate = Number(engine.getValue(activeGroup, "track_samplerate") || 0);
    for (var cue = 1; cue <= 8; cue++) {
        var frame = Number(engine.getValue(activeGroup, "hotcue_" + cue + "_position"));
        var normalized = frame >= 0 && duration > 0 && sampleRate > 0 ?
            frame / (duration * sampleRate) : -1;
        D2.sendState(group, "CUE" + cue, normalized.toFixed(6), !!force);
        D2.sendState(group, "CUECOLOR" + cue,
            D2.hotcueColor(activeGroup, cue), !!force);
    }
};

/* A track attach updates position and colour for all eight hotcues in one GUI
 * turn. Sending the complete marker set from every callback can submit
 * hundreds of SysEx packets in a few milliseconds and overflow PortMidi's
 * ALSA output queue. Coalesce the callback storm, preserve ordering and send
 * one cached marker snapshot after the authoritative track transaction. */
D2.scheduleDeckMarkers = function(group, force) {
    D2.markerRefreshForce[group] =
        D2.markerRefreshForce[group] || !!force;
    var token = (D2.markerRefreshToken[group] || 0) + 1;
    D2.markerRefreshToken[group] = token;
    if (D2.markerRefreshTimer[group])
        engine.stopTimer(D2.markerRefreshTimer[group]);
    D2.markerRefreshTimer[group] = engine.beginTimer(40, function() {
        if (D2.markerRefreshToken[group] !== token) return;
        D2.markerRefreshTimer[group] = null;
        var markerForce = D2.markerRefreshForce[group];
        D2.markerRefreshForce[group] = false;
        D2.publishDeckMarkers(group, markerForce);
        D2.refreshLEDs(group);
    }, true);
};

D2.publishTrackLoad = function(group) {
    var activeGroup = D2.activeGroup(group);
    var duration = Number(engine.getValue(activeGroup, "duration") || 0);
    var trackId = Math.round(Number(engine.getValue(activeGroup, "track_id") || 0));
    if (duration > 1) {
        /* EngineBuffer publishes the authoritative library identity before it
         * raises track_loaded.  Preserve this packet order: the bridge must
         * resolve title, location, waveform and beatmap by ID, never by a
         * rounded duration or by the currently highlighted Browse row. */
        if (trackId <= 0 && D2.pendingTrackId[group] > 0)
            trackId = D2.pendingTrackId[group];
        if (trackId > 0) {
            D2.sendState(group, "TRACKID", trackId, true);
        } else {
            var location = typeof engine.getTrackLocation === "function" ?
                engine.getTrackLocation(activeGroup) : "";
            if (!D2.sendTrackLocation(group, location))
                return false;
        }
        D2.sendState(group, "LOAD", duration.toFixed(3), true);
        /* TRACKID deliberately clears every track-derived bridge field. A
         * cached periodic snapshot would otherwise suppress unchanged values
         * and leave the new frame without its transport anchor, rate, play
         * state, visual key or beat window. Replay one coherent live snapshot
         * only after the complete TRACKID/LOCATION -> LOAD transaction. */
        D2.sendDeckState(group, true);
        D2.scheduleDeckMarkers(group, true);
        /* The bridge also invalidates its phase rows at identity start. Force
         * the current pair even when the four-step payload is unchanged. */
        D2.updatePhaseMeter(true);
        D2.pendingTrackId[group] = 0;
        D2.identityPublished[group] = true;
        return true;
    }
    return false;
};

D2.trackLoaded = function(trackGroup, value) {
    if (!value) {
        /* EngineBuffer may briefly publish an unload while replacing a track,
         * and a delayed failure from an older request must not erase a newer
         * successful deck.  Confirm the deck is still empty before clearing
         * the renderer identity. */
        engine.beginTimer(100, function() {
            if (engine.getValue(trackGroup, "track_loaded")) return;
            ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
            if (D2.activeGroup(surfaceGroup) === trackGroup) {
                D2.setBeatgridEdit(surfaceGroup, false);
                D2.pendingTrackId[surfaceGroup] = 0;
                    D2.identityPublished[surfaceGroup] = false;
                    D2.sendState(surfaceGroup, "TRACKID", 0, true);
                }
            });
        }, true);
        D2.updatePhaseMeter();
        return;
    }
    D2.phaseBeatStep[trackGroup] = 0;
    D2.phaseBeatActive[trackGroup] = false;
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) === trackGroup) {
            D2.setBeatgridEdit(surfaceGroup, false);
            D2.identityPublished[surfaceGroup] = false;
            /* Browse encoder press no longer guesses that LOAD succeeded.
             * Keep Browse visible for an offline/missing library entry and
             * return to the player only after Mixxx confirms track_loaded. */
            D2.setDisplayView(surfaceGroup, "DECK");
            /* The active zed skin exposes its authoritative full-screen page
             * through [Tab],current (0 = Player, 1 = Browse). Write that page
             * index directly so the result does not depend on tab-trigger
             * ordering. Return only after Mixxx confirms a readable track. */
            if (Number(engine.getValue("[Tab]", "current")) ===
                    D2.skinPage.browse) {
                engine.setValue("[Tab]", "current", D2.skinPage.player);
            }
            engine.beginTimer(120, function() {
                /* updateDisplays() may have completed the same identity while
                 * this settle timer was waiting. Never restart the bridge's
                 * asynchronous metadata/waveform generation for a track that
                 * is already published. */
                if (engine.getValue(trackGroup, "track_loaded") &&
                    !D2.identityPublished[surfaceGroup])
                    D2.publishTrackLoad(surfaceGroup);
            }, true);
        }
    });
    D2.updatePhaseMeter();
};

/* This event is emitted only from Mixxx's authoritative
 * LoadWhenDeckPlaying::Reject branch.  Do not infer rejection from
 * play_indicator, track_loaded, or a timeout: all three are ambiguous during
 * CUE preview and failed/offline library loads. */
D2.loadRejectedPlaying = function(trackGroup, sequence) {
    if (!(Number(sequence) > 0)) return;
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) === trackGroup)
            D2.sendState(surfaceGroup, "LOADREJECT", "PLAYING", true);
    });
};

/* Exact load outcomes are published by Mixxx itself. MissingFile and
 * NoSelection are decided synchronously by the library before a deck is
 * touched; FAILED is emitted only by the active EngineBuffer request. Keep
 * these events independent from track_loaded so a stale/unreadable playlist
 * entry can never be mistaken for a successful load. */
D2.loadOutcome = function(trackGroup, outcome, sequence) {
    if (!(Number(sequence) > 0)) return;
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.activeGroup(surfaceGroup) === trackGroup)
            D2.sendState(surfaceGroup, "LOADFAIL", outcome, true);
    });
};

D2.updateDisplays = function() {
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        var trackGroup = D2.activeGroup(surfaceGroup);
        if (!D2.identityPublished[surfaceGroup] &&
            engine.getValue(trackGroup, "track_loaded")) {
            D2.publishTrackLoad(surfaceGroup);
        }
    });
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

D2.phaseMasterGroup = function() {
    if (engine.getValue("[Channel1]", "sync_leader")) return "[Channel1]";
    if (engine.getValue("[Channel2]", "sync_leader")) return "[Channel2]";
    /* Without an explicit leader, use a stable A-over-B ordering on both
     * displays.  Reversing the rows per surface makes the same phase
     * relationship appear different on the two D2s. */
    return "[Channel1]";
};

D2.phaseFollowerGroup = function(masterGroup) {
    return masterGroup === "[Channel1]" ? "[Channel2]" : "[Channel1]";
};

D2.phaseValue = function(group) {
    var phase = Number(engine.getValue(group, "beat_distance"));
    if (!isFinite(phase)) return 0;
    phase = phase - Math.floor(phase);
    return phase < 0 ? phase + 1 : phase;
};

D2.updatePhaseMeter = function(force) {
    var masterGroup = D2.phaseMasterGroup();
    var followerGroup = D2.phaseFollowerGroup(masterGroup);
    var valid = engine.getValue(masterGroup, "track_loaded") &&
        engine.getValue(followerGroup, "track_loaded");
    var masterDeck = D2.deckNumber(masterGroup);
    var followerDeck = D2.deckNumber(followerGroup);
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        /* Both D2s show the same unambiguous pair: Sync Leader/reference on
         * the amber top row and follower on the white bottom row.  A leader
         * therefore never loses its meter by being compared with itself. */
        D2.sendState(surfaceGroup, "PHASE",
            "0.00000,0.00000," +
            D2.phaseBeatStep[masterGroup] + "," +
            D2.phaseBeatStep[followerGroup] + "," + (valid ? 1 : 0) + "," +
            masterDeck + "," + followerDeck, !!force);
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
            D2.scheduleDeckMarkers(surfaceGroup, false);
        }
    });
};

D2.setBeatgridEdit = function(group, enabled) {
    D2.beatgridEdit[group] = !!enabled;
    D2.sendState(group, "GRIDEDIT", enabled ? 1 : 0, true);
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
        else if (control === "bpm" || control === "visual_bpm")
            D2.sendState(surfaceGroup, "BPM",
                D2.displayBpm(trackGroup).toFixed(2), true);
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

/* TrackModel::SortColumnId values are a stable controller API in Mixxx.
 * Keep sorting inside Mixxx's library model so the zed table and both D2
 * displays always expose the same row order. */
D2.publishBrowseSortState = function(force) {
    var column = Math.round(Number(
        engine.getValue("[Library]", "sort_column")) || 0);
    var order = engine.getValue("[Library]", "sort_order") ? 1 : 0;
    var value = column + "," + order;
    D2.sendState("[Channel1]", "BROWSESORT", value, !!force);
    D2.sendState("[Channel2]", "BROWSESORT", value, !!force);
};

D2.browseSortChanged = function() {
    /* One user action can update both sort_column and sort_order. Collapse
     * those callbacks into one library refresh to protect MIDI/USB latency. */
    if (D2.browseSortTimer) engine.stopTimer(D2.browseSortTimer);
    D2.browseSortTimer = engine.beginTimer(40, function() {
        D2.browseSortTimer = null;
        D2.publishBrowseSortState(true);
        D2.refreshBrowseUntilSettled();
    }, true);
};

D2.sortBrowseByColumn = function(column) {
    engine.setValue("[Library]", "sort_column_toggle", column);
    /* Sorting/reselection is asynchronous in the table model. Publish the
     * indicator now and refresh the compact nine-row window after it settles. */
    D2.browseSortChanged();
};

D2.toggleBrowseSortOrder = function() {
    var descending = engine.getValue("[Library]", "sort_order") ? 1 : 0;
    engine.setValue("[Library]", "sort_order", descending ? 0 : 1);
    D2.browseSortChanged();
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

D2.publishSmartMenu = function(group, force) {
    D2.sendState(group, "SMARTMENU",
        (D2.smartMenu[group] ? 1 : 0) + "," + D2.smartIndex[group], !!force);
};

D2.closeSmartMenu = function(group) {
    D2.smartMenu[group] = false;
    D2.publishSmartMenu(group, true);
};

D2.toggleSmartMenu = function(group) {
    D2.smartMenu[group] = !D2.smartMenu[group];
    if (D2.smartMenu[group]) {
        D2.smartIndex[group] = 0;
        D2.libraryFocus = 0;
        engine.setValue("[Library]", "focused_widget", 3);
        D2.sendState(group, "BROWSEFOCUS", 0, true);
    }
    D2.publishSmartMenu(group, true);
};

D2.activateSmartList = function(group) {
    if (!D2.smartMenu[group]) return;
    var activeGroup = D2.activeGroup(group);
    var bpm = D2.displayBpm(activeGroup);
    var key = Number(engine.getValue(activeGroup, "visual_key")) || 0;
    engine.setValue("[Library]", "d2_smart_bpm", bpm);
    engine.setValue("[Library]", "d2_smart_key", key);
    engine.setValue("[Library]", "d2_smart_list", D2.smartIndex[group] + 1);
    engine.setValue("[Library]", "d2_smart_list", 0);
    D2.closeSmartMenu(group);
    D2.libraryFocus = 0;
    D2.sendState(group, "BROWSEFOCUS", 0, true);
    D2.refreshBrowseUntilSettled();
};

D2.browseTouch = function(channel, control, value, status, group) {
    if (value === 0x7F) {
        D2.closeSmartMenu(group);
        D2.libraryFocus = 0;
        engine.setValue("[Tab]", "current", D2.skinPage.browse);
        engine.setValue("[Library]", "focused_widget", 3);
        D2.sendState("[Channel1]", "BROWSEFOCUS", 0, true);
        D2.sendState("[Channel2]", "BROWSEFOCUS", 0, true);
        D2.publishBrowseSortState(true);
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
    if (D2.smartMenu[group]) {
        var direction = clockwise ? 1 : -1;
        D2.smartIndex[group] = (D2.smartIndex[group] + direction +
            D2.smartListCount) % D2.smartListCount;
        D2.publishSmartMenu(group, true);
        return;
    }
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

D2.sidebarActivate = function(channel, control, value, status, group) {
    if (!value) return;
    if (D2.smartMenu[group]) {
        D2.activateSmartList(group);
        return;
    }
    /* The bridge emits this handler only while the Library tree owns Browse.
     * Track-list presses remain the single native LoadSelectedTrack binding. */
    var isLeaf = engine.getValue("[Library]", "d2_sidebar_is_leaf") > 0;
    D2.pulse("[Library]", "d2_sidebar_activate");
    D2.libraryFocus = isLeaf ? 0 : 1;
    D2.sendState("[Channel1]", "BROWSEFOCUS", D2.libraryFocus, true);
    D2.sendState("[Channel2]", "BROWSEFOCUS", D2.libraryFocus, true);
    D2.refreshBrowseUntilSettled();
};

D2.backButton = function(channel, control, value, status, group) {
    if (!value) return;
    if (D2.smartMenu[group]) {
        D2.closeSmartMenu(group);
        return;
    }
    /* Select the exact widget. Tab traversal is skin-dependent and could land
     * on Search instead of the Sidebar in compact two-deck layouts. */
    D2.libraryFocus = D2.libraryFocus ? 0 : 1;
    engine.setValue("[Library]", "focused_widget", D2.libraryFocus ? 2 : 3);
    D2.sendState("[Channel1]", "BROWSEFOCUS", D2.libraryFocus, true);
    D2.sendState("[Channel2]", "BROWSEFOCUS", D2.libraryFocus, true);
    engine.beginTimer(30, D2.refreshBrowseModel, true);
};

D2.init = function(id, debugging) {
    D2.fxUnit["[Channel1]"] = 1;
    D2.fxUnit["[Channel2]"] = 2;
    D2.fxSettingsVisible["[Channel1]"] = false;
    D2.fxSettingsVisible["[Channel2]"] = false;
    D2.fxTouchMask["[Channel1]"] = 0;
    D2.fxTouchMask["[Channel2]"] = 0;
    D2.padHeldMask["[Channel1]"] = 0;
    D2.padHeldMask["[Channel2]"] = 0;
    D2.smartMenu["[Channel1]"] = false;
    D2.smartMenu["[Channel2]"] = false;
    D2.smartIndex["[Channel1]"] = 0;
    D2.smartIndex["[Channel2]"] = 0;
    D2.markerRefreshTimer["[Channel1]"] = null;
    D2.markerRefreshTimer["[Channel2]"] = null;
    D2.markerRefreshToken["[Channel1]"] = 0;
    D2.markerRefreshToken["[Channel2]"] = 0;
    D2.markerRefreshForce["[Channel1]"] = false;
    D2.markerRefreshForce["[Channel2]"] = false;
    D2.setDisplayView("[Channel1]", "DECK");
    D2.setDisplayView("[Channel2]", "DECK");
    D2.trackLoadConnections = [];
    D2.hotcueConnections = [];
    D2.keyConnections = [];
    D2.beatConnections = [];
    D2.liveStateConnections = [];
    D2.positionConnections = [];
    D2.loadRejectedConnections = [];
    D2.loadOutcomeConnections = [];
    D2.fxStateConnections = [];
    D2.samplerStateConnections = [];
    for (var deck = 1; deck <= 2; deck++) {
        (function(trackGroup) {
            D2.trackLoadConnections.push(engine.makeConnection(
                trackGroup, "track_loaded", function(value) {
                    D2.trackLoaded(trackGroup, value);
                }));
            D2.loadRejectedConnections.push(engine.makeConnection(
                trackGroup, "d2_load_rejected_playing_sequence", function(value) {
                    D2.loadRejectedPlaying(trackGroup, value);
                }));
            D2.loadOutcomeConnections.push(engine.makeConnection(
                trackGroup, "d2_load_missing_sequence", function(value) {
                    D2.loadOutcome(trackGroup, "MISSING", value);
                }));
            D2.loadOutcomeConnections.push(engine.makeConnection(
                trackGroup, "d2_load_no_selection_sequence", function(value) {
                    D2.loadOutcome(trackGroup, "NOSELECTION", value);
                }));
            D2.loadOutcomeConnections.push(engine.makeConnection(
                trackGroup, "d2_track_load_failed_sequence", function(value) {
                    D2.loadOutcome(trackGroup, "FAILED", value);
                }));
            for (var cue = 1; cue <= 8; cue++) {
                D2.hotcueConnections.push(engine.makeConnection(
                    trackGroup, "hotcue_" + cue + "_position", function() {
                        D2.hotcueChanged(trackGroup);
                    }));
                D2.hotcueConnections.push(engine.makeConnection(
                    trackGroup, "hotcue_" + cue + "_color", function() {
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
             "rate_ratio", "bpm", "visual_bpm"].forEach(function(control) {
                D2.liveStateConnections.push(engine.makeConnection(
                    trackGroup, control, function() {
                        D2.liveStateChanged(trackGroup, control,
                            engine.getValue(trackGroup, control));
                    }));
            });
        })("[Channel" + deck + "]");
    }
    for (var unitNumber = 1; unitNumber <= 2; unitNumber++) {
        (function(unit) {
            var unitGroup = "[EffectRack1_EffectUnit" + unit + "]";
            D2.fxStateConnections.push(engine.makeConnection(
                unitGroup, "mix", function() {
                    D2.forEachSurfaceUsingFxUnit(unit, function(surface) {
                        D2.sendState(surface, "FX1",
                            Number(engine.getParameter(unitGroup, "mix") || 0).toFixed(4));
                    });
                }));
            for (var effectNumber = 1; effectNumber <= 3; effectNumber++) {
                (function(effect) {
                    var effectGroup = "[EffectRack1_EffectUnit" + unit +
                                      "_Effect" + effect + "]";
                    ["meta", "enabled", "loaded_effect"].forEach(function(control) {
                        D2.fxStateConnections.push(engine.makeConnection(
                            effectGroup, control, function() {
                                D2.forEachSurfaceUsingFxUnit(unit, function(surface) {
                                    if (control === "meta") {
                                        D2.sendState(surface, "FX" + (effect + 1),
                                            Number(engine.getParameter(
                                                effectGroup, "meta") || 0).toFixed(4));
                                    } else if (control === "enabled") {
                                        D2.sendState(surface, "FXEN" + effect,
                                            engine.getValue(effectGroup, "enabled") ? 1 : 0);
                                        D2.refreshLEDs(surface);
                                    } else {
                                        D2.sendFxIdentity(surface, effect,
                                            engine.getValue(effectGroup,
                                                "loaded_effect"));
                                    }
                                });
                            }));
                    });
                })(effectNumber);
            }
            for (var routedDeck = 1; routedDeck <= 2; routedDeck++) {
                D2.fxStateConnections.push(engine.makeConnection(
                    unitGroup, "group_[Channel" + routedDeck + "]_enable",
                    function() {
                        D2.refreshLEDs("[Channel1]");
                        D2.refreshLEDs("[Channel2]");
                    }));
            }
        })(unitNumber);
    }
    /* All eight Remix pads can launch samplers, so every slot needs loaded
     * feedback.  Only the four physical performance faders have mute LEDs. */
    for (var sampler = 1; sampler <= 8; sampler++) {
        (function(samplerGroup, hasMuteFeedback) {
            D2.samplerStateConnections.push(engine.makeConnection(
                samplerGroup, "track_loaded", function() {
                    D2.refreshLEDs("[Channel1]");
                    D2.refreshLEDs("[Channel2]");
                }));
            if (hasMuteFeedback) {
                D2.samplerStateConnections.push(engine.makeConnection(
                    samplerGroup, "mute", function() {
                        D2.refreshLEDs("[Channel1]");
                        D2.refreshLEDs("[Channel2]");
                    }));
            }
        })("[Sampler" + sampler + "]", sampler <= 4);
    }
    D2.browseConnection = engine.makeConnection(
        "[Library]", "selected_track_id", D2.browseSelectionChanged);
    D2.browseSortConnections = [
        engine.makeConnection("[Library]", "sort_column",
            D2.browseSortChanged),
        engine.makeConnection("[Library]", "sort_order",
            D2.browseSortChanged)
    ];
    engine.beginTimer(250, function() {
        /* Startup can expose restored decks just after init(). The immediate
         * updateDisplays() path wins when state is already ready; this timer
         * is only a fallback and must not submit the same identity twice. */
        ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
            if (!D2.identityPublished[surfaceGroup])
                D2.publishTrackLoad(surfaceGroup);
        });
    }, true);
    D2.updateDisplays();
    /* One eager snapshot at startup gives both displays usable data. Every
     * subsequent selection change is debounced by browseSelectionChanged(). */
    D2.publishBrowseSnapshot(true);
    D2.publishBrowseSortState(true);
    /* Full state is event-driven; this is only a low-rate resilience snapshot.
     * Position is sent from the engine's actual playposition callback above. */
    D2.displayTimer = engine.beginTimer(500, D2.updateDisplays);
    D2.updatePhaseMeter();
    /* Beat connections publish changes immediately. This low-rate timer is
     * only a recovery snapshot and normally emits nothing due to caching. */
    D2.phaseTimer = engine.beginTimer(500, D2.updatePhaseMeter);
};

D2.shutdown = function() {
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        var activeGroup = D2.activeGroup(surfaceGroup);
        var state = D2.touchStripState[surfaceGroup];
        if (state && state.mode === "SCRATCH")
            engine.scratchDisable(state.deck, false);
        else if (state && state.mode === "BEND")
            engine.setValue(state.targetGroup, "jog", 0);
        D2.touchStripState[surfaceGroup] = null;
        D2.touchStripPressed[surfaceGroup] = false;
        if (D2.cuePreviewing[surfaceGroup])
            engine.setValue(activeGroup, "cue_preview", 0);
        D2.cueHeld[surfaceGroup] = false;
        D2.cuePreviewing[surfaceGroup] = false;
        var loopBeats = [0.25, 0.5, 1, 2, 4, 8, 16, 32];
        for (var pad = 1; pad <= 8; pad++) {
            engine.setValue(activeGroup, "hotcue_" + pad + "_activate", 0);
            engine.setValue(activeGroup,
                "beatlooproll_" + loopBeats[pad - 1] + "_activate", 0);
        }
        D2.fxTouchMask[surfaceGroup] = 0;
        D2.fxSettingsVisible[surfaceGroup] = false;
        D2.screenEncoderTouched[surfaceGroup] = 0;
        D2.faderTouched[surfaceGroup] = 0;
        D2.loopTouched[surfaceGroup] = false;
        D2.padHeldMask[surfaceGroup] = 0;
        D2.closeSmartMenu(surfaceGroup);
        D2.setBeatgridEdit(surfaceGroup, false);
        D2.sendState(surfaceGroup, "FXTOUCH", 0, true);
        D2.sendState(surfaceGroup, "LEDOFF", 1, true);
    });
    if (D2.trackLoadConnections) {
        for (var i = 0; i < D2.trackLoadConnections.length; i++)
            D2.trackLoadConnections[i].disconnect();
    }
    if (D2.loadRejectedConnections) {
        for (var rejectedConnection = 0;
             rejectedConnection < D2.loadRejectedConnections.length;
             rejectedConnection++)
            D2.loadRejectedConnections[rejectedConnection].disconnect();
    }
    if (D2.loadOutcomeConnections) {
        for (var outcomeConnection = 0;
             outcomeConnection < D2.loadOutcomeConnections.length;
             outcomeConnection++)
            D2.loadOutcomeConnections[outcomeConnection].disconnect();
    }
    if (D2.browseConnection) D2.browseConnection.disconnect();
    if (D2.browseSortConnections) {
        for (var sortConnection = 0;
             sortConnection < D2.browseSortConnections.length;
             sortConnection++)
            D2.browseSortConnections[sortConnection].disconnect();
    }
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
    if (D2.fxStateConnections) {
        for (var fxStateConnection = 0;
             fxStateConnection < D2.fxStateConnections.length;
             fxStateConnection++)
            D2.fxStateConnections[fxStateConnection].disconnect();
    }
    if (D2.samplerStateConnections) {
        for (var samplerStateConnection = 0;
             samplerStateConnection < D2.samplerStateConnections.length;
             samplerStateConnection++)
            D2.samplerStateConnections[samplerStateConnection].disconnect();
    }
    if (D2.displayTimer) engine.stopTimer(D2.displayTimer);
    if (D2.phaseTimer) engine.stopTimer(D2.phaseTimer);
    if (D2.browseSnapshotTimer) engine.stopTimer(D2.browseSnapshotTimer);
    if (D2.browseSortTimer) engine.stopTimer(D2.browseSortTimer);
    ["[Channel1]", "[Channel2]"].forEach(function(surfaceGroup) {
        if (D2.markerRefreshTimer[surfaceGroup])
            engine.stopTimer(D2.markerRefreshTimer[surfaceGroup]);
        D2.markerRefreshTimer[surfaceGroup] = null;
        D2.markerRefreshForce[surfaceGroup] = false;
        D2.markerRefreshToken[surfaceGroup]++;
    });
    D2.trackLoadConnections = null;
    D2.loadRejectedConnections = null;
    D2.loadOutcomeConnections = null;
    D2.hotcueConnections = null;
    D2.keyConnections = null;
    D2.beatConnections = null;
    D2.positionConnections = null;
    D2.liveStateConnections = null;
    D2.fxStateConnections = null;
    D2.samplerStateConnections = null;
    D2.browseConnection = null;
    D2.browseSortConnections = null;
    D2.displayTimer = null;
    D2.phaseTimer = null;
    D2.browseSnapshotTimer = null;
    D2.browseSortTimer = null;
};

D2.shiftButton = function(channel, control, value, status, group) {
    D2.shiftPressed[group] = value === 0x7F;
    D2.refreshLEDs(group);
};

D2.syncButton = function(channel, control, value, status, group) {
    if (!value) return;
    var activeGroup = D2.activeGroup(group);
    if (D2.shiftPressed[group])
        D2.pulse(activeGroup, "beatsync");
    else
        engine.setValue(activeGroup, "sync_enabled",
            !engine.getValue(activeGroup, "sync_enabled"));
    /* Mixxx can select the Sync Leader just after sync_enabled changes. Send
     * one immediate update and one settled snapshot so both D2s change row
     * labels/order together instead of waiting for the next beat. */
    D2.updatePhaseMeter();
    engine.beginTimer(60, D2.updatePhaseMeter, true);
    D2.refreshLEDs(group);
};

D2.deckButton = function(channel, control, value, status, group) {
    if (!value) return;
    /* This custom Mixxx build instantiates two player decks. Keep the native
     * DECK button deterministic: it always dismisses overlays, restores the
     * corresponding D1/D2 player and closes the global zed Browse page. */
    engine.setValue("[Tab]", "current", D2.skinPage.player);
    D2.setBeatgridEdit(group, false);
    D2.setDisplayView(group, "DECK");
    D2.sendDeckState(group);
};

D2.fxAssignButton = function(channel, control, value, status, group) {
    if (!value) return;
    var targetDeck = control - 0x24 + 1;
    var ownDeck = D2.deckNumber(group);
    /* Each physical surface owns exactly one deck/unit pair: left D2 controls
     * Deck A / Unit 1, right D2 controls Deck B / Unit 2. Cross-assignment
     * would make the two surfaces silently operate on each other's audio. */
    if (targetDeck !== ownDeck) return;
    var targetGroup = "[Channel" + targetDeck + "]";
    var unitGroup = D2.effectUnitGroup(group);
    var key = "group_" + targetGroup + "_enable";
    engine.setValue(unitGroup, key, !engine.getValue(unitGroup, key));
    D2.refreshLEDs(group);
};

D2.fxSelectButton = function(channel, control, value, status, group) {
    if (!value) return;
    D2.fxUnit[group] = D2.deckNumber(group);
    D2.fxSettingsVisible[group] = !D2.fxSettingsVisible[group];
    D2.fxTouchMask[group] = 0;
    D2.publishFxOverlay(group, true);
    D2.sendFxUnitState(group);
    D2.refreshLEDs(group);
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
    var padBit = 1 << (pad - 1);
    if (value) D2.padHeldMask[group] |= padBit;
    else D2.padHeldMask[group] &= ~padBit;
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
        if (value) D2.scheduleDeckMarkers(group, false);
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
    if (!value) {
        /* Beatjump has no held Mixxx CO. Its local edge state drives both the
         * physical RGB pad and the on-screen 4x2 feedback cell. */
        if (mode === "BEATJUMP") D2.refreshLEDs(group);
        return;
    }
    if (mode === "LOOP") {
        controlName = "beatloop_" + loopBeats[pad - 1] + "_toggle";
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
    /* Browse-context controls: L1 Title, L2 BPM, L3 Key and L4 direction.
     * The same physical buttons retain their established player functions
     * as soon as the zed Player page is restored. */
    if (Number(engine.getValue("[Tab]", "current")) ===
            D2.skinPage.browse) {
        if (button === 0)
            D2.sortBrowseByColumn(D2.browseSortColumn.title);
        else if (button === 1)
            D2.sortBrowseByColumn(D2.browseSortColumn.bpm);
        else if (button === 2)
            D2.sortBrowseByColumn(D2.browseSortColumn.key);
        else if (button === 3)
            D2.toggleBrowseSortOrder();
        return;
    }
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
    if (Number(engine.getValue("[Tab]", "current")) ===
            D2.skinPage.browse) {
        if (button === 0) D2.toggleSmartMenu(group);
        return;
    }
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
    var column = control - 0x28;
    if (column === 0) {
        var ownDeck = D2.deckNumber(group);
        var unitGroup = D2.effectUnitGroup(group);
        var routeKey = "group_[Channel" + ownDeck + "]_enable";
        engine.setValue(unitGroup, routeKey,
            !engine.getValue(unitGroup, routeKey));
    } else if (column >= 1 && column <= 3) {
        var effectGroup = D2.effectGroup(group, column);
        if (D2.shiftPressed[group]) {
            /* Keep normal FX buttons as enable toggles. SHIFT adds a separate,
             * deterministic selector gesture without stealing any approved
             * control: each press advances that slot by one visible effect. */
            D2.showFxSelection(group, column);
            D2.pulse(effectGroup, "next_effect");
            return;
        }
        engine.setValue(effectGroup, "enabled", !engine.getValue(effectGroup, "enabled"));
    }
    /* An FX push must never leave the capacitive overlay latched on screen. */
    D2.fxTouchMask[group] = 0;
    D2.publishFxOverlay(group, true);
    D2.sendDeckState(group);
};

D2.fxTouch = function(channel, control, value, status, group) {
    var slot = control - 0x2C;
    var mask = D2.fxTouchMask[group] || 0;
    if (value) mask |= 1 << slot;
    else mask &= ~(1 << slot);
    D2.fxTouchMask[group] = mask;
    D2.publishFxOverlay(group, true);
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
    /* Mixxx 2.5 exposes no sampler-fader touch CO and the approved Player
     * layout has no matching overlay. Keep the hardware state locally but do
     * not waste the shared MIDI/USB queue on an unconsumed display packet. */
};

D2.touchStripTouch = function(channel, control, value, status, group) {
    var pressed = value !== 0;
    D2.touchStripPressed[group] = pressed;
    if (pressed) {
        var targetGroup = D2.activeGroup(group);
        var deck = D2.activeDeck[group];
        var playing = engine.getValue(targetGroup, "play_indicator") ||
                      engine.getValue(targetGroup, "play");
        var mode = D2.shiftPressed[group] ? "SEEK" :
                   (playing ? "BEND" : "SCRATCH");
        D2.touchStripState[group] = {
            pressed: true,
            mode: mode,
            targetGroup: targetGroup,
            deck: deck,
            lastValue: -1
        };
        /* A stopped Traktor deck treats an unshifted strip as a virtual
         * record: touch holds it and movement scratches/backspins. */
        if (mode === "SCRATCH")
            engine.scratchEnable(deck, 128, 33.333333, 0.125,
                                 0.00390625, true);
        return;
    }

    var state = D2.touchStripState[group];
    if (state && state.mode === "SCRATCH")
        engine.scratchDisable(state.deck, true);
    else if (state && state.mode === "BEND")
        engine.setValue(state.targetGroup, "jog", 0);
    D2.touchStripState[group] = null;
};

D2.touchStrip = function(channel, control, value, status, group) {
    var state = D2.touchStripState[group];
    if (!state || !state.pressed) return;
    var sample = Math.max(0, Math.min(127, Number(value) || 0));
    var normalized = sample / 127;

    /* The mode and target deck are locked at touch-down. This prevents a
     * mid-gesture SHIFT/deck change from redirecting audio unexpectedly. */
    if (state.mode === "SEEK") {
        state.lastValue = sample;
        engine.setValue(state.targetGroup, "playposition", normalized);
        return;
    }

    var previous = state.lastValue;
    state.lastValue = sample;
    if (previous < 0) return;
    var delta = sample - previous;
    if (delta === 0) return;

    if (state.mode === "BEND") {
        delta = Math.max(-8, Math.min(8, delta));
        /* Traktor's default direction emulates nudging vinyl: left speeds up,
         * right slows down. Mixxx positive jog values speed the deck up. */
        engine.setValue(state.targetGroup, "jog",
                        -delta * D2.touchStripBendScale);
    } else if (state.mode === "SCRATCH") {
        delta = Math.max(-32, Math.min(32, delta));
        engine.scratchTick(state.deck, -delta);
    }
};

D2.captureButton = function(channel, control, value, status, group) {
    if (!value) return;
    engine.setValue("[Library]", "AutoDjAddBottom", 1);
    engine.setValue("[Library]", "AutoDjAddBottom", 0);
};

D2.usbOpenButton = function(channel, control, value, status, group) {
    if (!value) return;
    D2.setDisplayView(group, "BROWSE");
    D2.pulse("[Library]", "d2_usb_open");
    D2.refreshBrowseUntilSettled();
};

D2.editButton = function(channel, control, value, status, group) {
    if (!value) return;
    D2.setBeatgridEdit(group, !D2.beatgridEdit[group]);
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
    /* Turning remains fully functional. Touch itself has no Mixxx CO, so it
     * stays local instead of generating a dead SysEx message. */
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
    /* Loop size and active state are already always visible on the Player.
     * Do not publish an unconsumed duplicate touch packet. */
};
