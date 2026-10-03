# gdb script for tools/regress/regress.py's fps60 check (the plaza gate run).
#
# Run under gdb on a plaza load at scenario 5 (SMS_WARP=1,5,0): once the gate
# (TModelGate "Gate") is loaded, it sets the flag that lets it open, and after
# 600 of the gate's perform calls puts Mario next to it. When the gate's wind
# comes out it puts Mario in the gate's capture point and makes him jump, and
# reports when TMario::warpIn starts and the setNextStage that follows, then
# kills the game. Frames are counted in TMarDirector::direct calls from the
# moment Mario is placed. regress.py reads the lines starting with "gate:".
import gdb

state = {'gate': None, 'hits': 0, 'tele': False, 'jump': 0, 'frames': 0}


def report(msg):
    print('gate: ' + msg, flush=True)


def fail(msg):
    report('error: ' + msg)
    gdb.execute('kill')
    return True


class LoadAfter(gdb.Breakpoint):
    def stop(self):
        try:
            this = gdb.parse_and_eval('this')
            if this['mName'].string() == 'Gate':
                state['gate'] = int(this)
                gdb.execute('set var TFlagManager::smInstance->mCardBools[112] = '
                            'TFlagManager::smInstance->mCardBools[112] | 0x30')
                report('loaded, flag 0x10385 set')
        except gdb.error as e:
            return fail('TModelGate::loadAfter: %s' % e)
        return False


class Direct(gdb.Breakpoint):
    def stop(self):
        if state['tele']:
            state['frames'] += 1
        return False


class NextStage(gdb.Breakpoint):
    def stop(self):
        if not state['tele']:
            return False
        try:
            report('setNextStage(%#x, %s) after %d frames' % (
                int(gdb.parse_and_eval('param_1')), gdb.parse_and_eval('param_2'), state['frames']))
        except gdb.error as e:
            return fail('TMarDirector::setNextStage: %s' % e)
        gdb.execute('kill')
        return True


class WarpIn(gdb.Breakpoint):
    def stop(self):
        if state['jump'] == 1:
            report('warpIn frame %d' % state['frames'])
            state['jump'] = 2
        return False


class Perform(gdb.Breakpoint):
    def stop(self):
        if state['gate'] is None:
            return False
        try:
            if int(gdb.parse_and_eval('this')) != state['gate']:
                return False
            state['hits'] += 1
            if state['hits'] < 600:
                return False
            g = gdb.parse_and_eval('this')
            cue = int(gdb.parse_and_eval('cue'))
            if not state['tele']:
                state['tele'] = True
                p = g['mPosition']
                gdb.execute('set var gpMarioPos->x = %f' % float(p['x']))
                gdb.execute('set var gpMarioPos->y = %f' % (float(p['y']) + 200))
                gdb.execute('set var gpMarioPos->z = %f' % (float(p['z']) + 600))
                report('Mario placed 632 units from the gate')
            wind = int(g['mWindTimeLeft'])
            rate = float(g['mOpenRate'])
            if wind > 0 and state['jump'] == 0 and (cue & 1):
                ac = g['unkAC']
                report('frame %d: wind out (rate %f wind %d), Mario jumps into the gate'
                       % (state['frames'], rate, wind))
                gdb.execute('set var gpMarioPos->x = %f' % float(ac['x']))
                gdb.execute('set var gpMarioPos->y = %f' % float(ac['y']))
                gdb.execute('set var gpMarioPos->z = %f' % float(ac['z']))
                gdb.execute('set var gpMarioOriginal->mStatus = MARIO_STATUS_JUMP')
                state['jump'] = 1
            if state['frames'] > 400:
                report('gave up after 400 frames: rate %f wind %d' % (rate, wind))
                gdb.execute('kill')
                return True
        except gdb.error as e:
            return fail('TModelGate::perform: %s' % e)
        return False


LoadAfter('TModelGate::loadAfter')
Perform('TModelGate::perform')
Direct('TMarDirector::direct')
NextStage('TMarDirector::setNextStage')
WarpIn('TMario::warpIn')
