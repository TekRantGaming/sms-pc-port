# gdb script for tools/regress/regress.py's ecl-luigi and ecl-piantissimo runs.
#
# Eclipse loads the player's archive from sCharacterIDList[0] (mario.szs,
# luigi.szs, piantissimo.szs) when a stage starts, and its character select
# screen, which sets it, only comes up once a save has unlocked the others.
# This sets it to REGRESS_CHARACTER (1 Luigi, 2 Piantissimo) as the stage
# loads. The run is the Tutorial's input, which makes the character jump.
#
# It reports, on lines starting with "regress: " that regress.py keeps as
# items:
# - each parameter the moveset's better_movement.prm gives (onPlayerInit),
#   read through the game's TParamT<T>::load, which converts it from
#   big-endian (a module reading it with a copy of its own would not stop
#   here, and the items would be missing);
# - the player's jump parameters after the moveset has scaled them;
# - each jump: the vertical speed at take-off, the height gained, the frames
#   in the air and the lowest vertical speed (counted in jumpProcess calls).
import os

import gdb

CHARACTER = int(os.environ.get('REGRESS_CHARACTER', '1'))
state = {'init': False, 'jumps': 0, 'jump': None}


def report(key, value):
    print('regress: %s %s' % (key, value), flush=True)


def member(value, *names):
    # Two classes are named TMario in the binary, the decomp's and
    # SunshineHeaderInterface's (the same layout, other member names), and
    # gdb may resolve either: take whichever name it has.
    for n in names:
        try:
            return value[n]
        except gdb.error:
            pass
    raise gdb.error('no member %s' % '/'.join(names))


def fmt(v):
    return '%.4f' % float(v)


class Archives(gdb.Breakpoint):
    def stop(self):
        gdb.execute('set var SME::TGlobals::sCharacterIDList[0] = (SME::CharacterID)%d' % CHARACTER)
        report('character', str(gdb.parse_and_eval('SME::TGlobals::sCharacterIDList[0]')).split('::')[-1])
        return False


class LoadDone(gdb.FinishBreakpoint):
    def __init__(self, this, ty):
        super().__init__(gdb.newest_frame(), internal=True)
        self.this, self.ty = this, ty

    def stop(self):
        p = gdb.parse_and_eval('*(TParamT<%s>*)%d' % (self.ty, self.this))
        v = member(p, 'value', 'mValue')
        report('prm:' + member(p, 'name', 'mName').string(), int(v) if self.ty != 'float' else fmt(v))
        return False

    def out_of_scope(self):
        pass


class Load(gdb.Breakpoint):
    def __init__(self, ty):
        super().__init__('TParamT<%s>::load' % ty)
        self.ty = ty

    def stop(self):
        if state['init']:
            LoadDone(int(gdb.parse_and_eval('this')), self.ty)
        return False


class InitDone(gdb.FinishBreakpoint):
    def __init__(self, player):
        super().__init__(gdb.newest_frame(), internal=True)
        self.player = player

    def stop(self):
        state['init'] = False
        m = gdb.parse_and_eval('(TMario*)%d' % self.player)
        jp = m['mJumpParams']
        for f in ('mGravity', 'mPopUpSpeedY', 'mJumpingMax'):
            report('mario:' + f, fmt(member(jp[f], 'value', 'mValue')))
        report('mario:mRunningMax', fmt(member(m['mDeParams']['mRunningMax'], 'value', 'mValue')))
        return False

    def out_of_scope(self):
        state['init'] = False


class PlayerInit(gdb.Breakpoint):
    def stop(self):
        state['init'] = True
        InitDone(int(gdb.parse_and_eval('player')))
        return False


class JumpDone(gdb.FinishBreakpoint):
    def __init__(self):
        super().__init__(gdb.newest_frame(), internal=True)

    def stop(self):
        j = state['jump']
        if j is None:
            return False
        if int(self.return_value) != 0:
            state['jumps'] += 1
            report('jump%d' % state['jumps'], 'vy0=%s,rise=%s,frames=%d,minvy=%s' % (
                fmt(j['vy0']), fmt(j['top'] - j['y0']), j['frames'], fmt(j['minvy'])))
            state['jump'] = None
        return False

    def out_of_scope(self):
        pass


class JumpProcess(gdb.Breakpoint):
    def stop(self):
        y = float(gdb.parse_and_eval('gpMarioPos->y'))
        vy = float(member(gdb.parse_and_eval('this'), 'mVel', 'mSpeed')['y'])
        j = state['jump']
        if j is None:
            j = state['jump'] = {'y0': y, 'vy0': vy, 'top': y, 'frames': 0, 'minvy': vy}
        j['top'] = max(j['top'], y)
        j['minvy'] = min(j['minvy'], vy)
        j['frames'] += 1
        JumpDone()
        return False


Archives('initCharacterArchives')
Load('float')
Load('unsigned char')
PlayerInit('onPlayerInit')
JumpProcess('TMario::jumpProcess')
