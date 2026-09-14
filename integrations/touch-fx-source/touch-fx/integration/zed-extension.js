TouchFX.unit = '[EffectRack1_EffectUnit3]';
TouchFX.zedReady = false;
TouchFX.skinConnection = null;
TouchFX.skinPrevious = 0;
TouchFX.baseInit = TouchFX.init;
TouchFX.baseShutdown = TouchFX.shutdown;
TouchFX.baseWatchdog = TouchFX.watchdog;
TouchFX.baseGate = TouchFX.gate;
TouchFX.pendingEffect = null;
TouchFX.effectBlocked = false;

TouchFX.validChannel = function(channel) {
    return TouchFX.zedReady && (channel === 0 || channel === 1);
};

TouchFX.route = function(channel, force) {
    if (!TouchFX.validChannel(channel)) return false;
    if (force || channel !== TouchFX.channel) {
        TouchFX.off();
        for (var deck = 1; deck <= 2; deck++) {
            engine.setValue(TouchFX.unit, 'group_[Channel'+deck+']_enable', 0);
        }
        engine.setValue(TouchFX.unit, 'group_[Channel'+(channel+1)+']_enable', 1);
        TouchFX.channel = channel;
    }
    return true;
};

TouchFX.bindSkin = function() {
    if (!TouchFX.zedReady) return;
    if (TouchFX.skinConnection) {
        if (TouchFX.skinConnection.isConnected !== false) return;
        TouchFX.skinConnection.disconnect();
        TouchFX.skinConnection = null;
    }
    var connection = engine.makeConnection('[Skin]', 'touch_fx', function(value) {
        if (!TouchFX.zedReady || TouchFX.skinConnection !== connection ||
                connection.isConnected === false || value === TouchFX.skinPrevious) return;
        TouchFX.skinPrevious = value;
        midi.sendShortMsg(0xB0, 120, 127);
    });
    if (!connection || connection.isConnected === false) return;
    TouchFX.skinConnection = connection;
    TouchFX.skinPrevious = engine.getValue('[Skin]', 'touch_fx');
};

TouchFX.init = function(id, debugging) {
    TouchFX.shutdown();
    if (!(engine.getValue(TouchFX.unit, 'num_effectslots') > 0)) {
        print('ZED Touch FX: Effect Unit 3 unavailable; no effect controls changed.');
        return;
    }
    TouchFX.zedReady = true;
    TouchFX.effectBlocked = false;
    TouchFX.baseInit(id, debugging);
    TouchFX.bindSkin();
};

TouchFX.watchdog = function() {
    TouchFX.baseWatchdog();
    TouchFX.bindSkin();
    TouchFX.reportEffect();
};

TouchFX.reportEffect = function() {
    if (!TouchFX.zedReady) return;
    TouchFX.finishEffectSelection();
    var slot = '[EffectRack1_EffectUnit3_Effect1]';
    var index = engine.getValue(slot, 'loaded_effect');
    var loaded = engine.getValue(slot, 'loaded') > 0;
    var enabled = engine.getValue(slot, 'enabled') > 0;
    var flags = (engine.getValue(TouchFX.unit, 'enabled') > 0 ? 1 : 0) |
        (enabled ? 2 : 0) | (loaded ? 16 : 0) |
        (engine.getValue(TouchFX.unit, 'group_[Channel1]_enable') > 0 ? 4 : 0) |
        (engine.getValue(TouchFX.unit, 'group_[Channel2]_enable') > 0 ? 8 : 0);
    midi.sendShortMsg(0xB0, 15, flags);
    if (TouchFX.effectBlocked || TouchFX.macroBusy || !loaded || !enabled) index = 0;
    if (index >= 0 && index <= 127 && Math.floor(index) === index) {
        midi.sendShortMsg(0xB0, 14, index);
    }
};

TouchFX.finishEffectSelection = function() {
    var pending = TouchFX.pendingEffect;
    if (!pending) return;
    if (Date.now() >= pending.deadline || Date.now() < pending.started) {
        TouchFX.pendingEffect = null;
        TouchFX.off();
        engine.setValue('[EffectRack1_EffectUnit3_Effect1]', 'enabled', 0);
        return;
    }
    var slot = '[EffectRack1_EffectUnit3_Effect1]';
    if (engine.getValue(slot, 'loaded_effect') !== pending.index ||
            !engine.getValue(slot, 'loaded')) return;
    if (!pending.enabling) {
        engine.setValue(slot, 'enabled', 1);
        pending.enabling = true;
    }
    if (engine.getValue(slot, 'enabled') > 0) {
        TouchFX.pendingEffect = null;
        TouchFX.effectBlocked = false;
    }
};

TouchFX.gate = function(channel, control, value, status, group) {
    if (TouchFX.effectBlocked && (status & 0xF0) === 0x90 && value > 0) return;
    TouchFX.baseGate(channel, control, value, status, group);
};

TouchFX.selectEffect = function(channel, control, value, status, group) {
    if (control !== 13 || !TouchFX.validChannel(channel) ||
            value < 1 || value > 127 || Math.floor(value) !== value) return;
    if (TouchFX.cancelMacro) TouchFX.cancelMacro();
    TouchFX.route(channel, false);
    TouchFX.off();
    var slot = '[EffectRack1_EffectUnit3_Effect1]';
    TouchFX.effectBlocked = true;
    TouchFX.pendingEffect = {index: value, started: Date.now(), deadline: Date.now()+1500, enabling: false};
    engine.setValue(slot, 'enabled', 0);
    var count = engine.getValue(TouchFX.unit, 'num_effectslots');
    for (var index = 2; index <= count; index++) {
        engine.setValue('[EffectRack1_EffectUnit3_Effect'+index+']', 'enabled', 0);
    }
    engine.setParameter(TouchFX.unit, 'super1', 64/127);
    engine.setParameter(TouchFX.unit, 'mix', 0);
    engine.setValue(slot, 'loaded_effect', value);
    TouchFX.reportEffect();
};

TouchFX.shutdown = function() {
    TouchFX.pendingEffect = null;
    if (TouchFX.zedReady) TouchFX.baseShutdown();
    TouchFX.zedReady = false;
    if (TouchFX.skinConnection) TouchFX.skinConnection.disconnect();
    TouchFX.skinConnection = null;
};
