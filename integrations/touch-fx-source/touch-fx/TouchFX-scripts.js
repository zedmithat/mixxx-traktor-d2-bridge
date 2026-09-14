var TouchFX = {};

TouchFX.unit = '[EffectRack1_EffectUnit1]';
TouchFX.channel = 0;
TouchFX.active = false;
TouchFX.timer = 0;
TouchFX.lastSeen = 0;
TouchFX.timeoutMs = 1500;

TouchFX.validChannel = function(channel) {
    return channel === 0 || channel === 1 || channel === 2 || channel === 3;
};

TouchFX.off = function() {
    engine.setValue(TouchFX.unit, 'enabled', 0);
    TouchFX.active = false;
};

TouchFX.route = function(channel, force) {
    if (!TouchFX.validChannel(channel)) {
        return false;
    }
    if (force || channel !== TouchFX.channel) {
        TouchFX.off();
        for (var deck = 1; deck <= 4; deck++) {
            engine.setValue(TouchFX.unit, 'group_[Channel'+deck+']_enable', 0);
        }
        engine.setValue(TouchFX.unit, 'group_[Channel'+(channel+1)+']_enable', 1);
        TouchFX.channel = channel;
    }
    return true;
};

TouchFX.watchdog = function() {
    var elapsed = Date.now()-TouchFX.lastSeen;
    if (TouchFX.active && (elapsed >= TouchFX.timeoutMs || elapsed < 0)) {
        TouchFX.off();
    }
    if (TouchFX.macroWatchdog) TouchFX.macroWatchdog();
};

TouchFX.init = function(id, debugging) {
    if (TouchFX.cancelMacro) TouchFX.cancelMacro();
    TouchFX.off();
    if (TouchFX.timer) {
        engine.stopTimer(TouchFX.timer);
    }
    engine.setValue(TouchFX.unit, 'group_[Master]_enable', 0);
    engine.setValue(TouchFX.unit, 'group_[Headphone]_enable', 0);
    var samplers = engine.getValue('[App]', 'num_samplers');
    for (var sampler = 1; sampler <= samplers; sampler++) {
        engine.setValue(TouchFX.unit, 'group_[Sampler'+sampler+']_enable', 0);
    }
    TouchFX.route(0, true);
    TouchFX.lastSeen = Date.now();
    TouchFX.timer = engine.beginTimer(250, TouchFX.watchdog);
    if (!TouchFX.timer) {
        print('TouchFX: watchdog timer unavailable; FX ON is blocked.');
    }
};

TouchFX.shutdown = function() {
    TouchFX.off();
    if (TouchFX.timer) {
        engine.stopTimer(TouchFX.timer);
        TouchFX.timer = 0;
    }
};

TouchFX.selectDeck = function(channel, control, value, status, group) {
    if (value > 0 && TouchFX.route(channel, false)) {
        TouchFX.lastSeen = Date.now();
    }
};

TouchFX.axis = function(channel, control, value, status, group) {
    if ((control !== 10 && control !== 11) || !TouchFX.route(channel, false)) {
        return;
    }
    var normalized = Math.max(0, Math.min(127, value))/127;
    if (TouchFX.macroBusy) return;
    if (TouchFX.macroId && TouchFX.macroAxis) {
        TouchFX.macroAxis(control, normalized);
        TouchFX.lastSeen = Date.now();
        return;
    }
    engine.setParameter(TouchFX.unit, control === 10 ? 'super1' : 'mix', normalized);
    TouchFX.lastSeen = Date.now();
};

TouchFX.gate = function(channel, control, value, status, group) {
    if (!TouchFX.validChannel(channel) || control !== 60) {
        return;
    }
    var messageType = status & 0xF0;
    var noteOn = messageType === 0x90 && value > 0;
    var noteOff = messageType === 0x80 || (messageType === 0x90 && value === 0);
    if (noteOn && TouchFX.timer) {
        if (TouchFX.macroBusy) return;
        TouchFX.route(channel, false);
        TouchFX.lastSeen = Date.now();
        TouchFX.active = true;
        if (TouchFX.macroStart) TouchFX.macroStart();
        engine.setValue(TouchFX.unit, 'enabled', 1);
    } else if (noteOff && channel === TouchFX.channel) {
        TouchFX.off();
    }
};

TouchFX.heartbeat = function(channel, control, value, status, group) {
    if (TouchFX.validChannel(channel) && channel === TouchFX.channel && control === 119) {
        TouchFX.lastSeen = Date.now();
        if (value === 0) {
            TouchFX.off();
        }
    }
};
