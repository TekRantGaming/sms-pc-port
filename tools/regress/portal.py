# Run under gdb with SMS_SKIP_MOVIES=1, SMS_WARP=1,5,0 and GATE_AP input.
# Check that cutscenes skip, the portal placeholder keeps its authored I4 tile, and
# successive decoded frames bind all three movie planes at their own sizes.
import gdb

state = {'gate': None, 'placeholder': False, 'skipped': False, 'frames': set()}


def fail(message):
    print('portal: error: ' + message, flush=True)
    return True


class Load(gdb.Breakpoint):
    def stop(self):
        gate = gdb.parse_and_eval('this')
        if gate['mName'].string(encoding='cp932', errors='replace') == 'Gate':
            state['gate'] = int(gate)
            gdb.execute('set var TFlagManager::smInstance->mCardBools[112] |= 0x30')
        return False


class Played(gdb.FinishBreakpoint):
    def stop(self):
        player = gdb.parse_and_eval('ActivePlayer')
        if not int(gdb.parse_and_eval('port_skip_movies')):
            return fail('skip setting changed during playback')
        loop = int(player['playFlag']) & 1
        expected = 2 if loop else 3
        if int(player['state']) != expected:
            return fail('movie state %d; expected %d (loop=%d)' % (
                int(player['state']), expected, loop))
        if not loop:
            state['skipped'] = True
        return False


class Play(gdb.Breakpoint):
    def stop(self):
        player = gdb.parse_and_eval('ActivePlayer')
        if int(player['open']) and int(player['state']) in (1, 4):
            Played(gdb.newest_frame(), internal=True)
        return False


class Bound(gdb.FinishBreakpoint):
    def stop(self):
        gate = gdb.parse_and_eval('(TModelGate*)%d' % state['gate'])
        resources = gate['unk78']['mModel']['mModelData']['unkAC']['mResources']
        player = gdb.parse_and_eval('ActivePlayer')
        frame = player['dispTextureSet'].dereference()
        width, height = int(player['videoInfo']['xSize']), int(player['videoInfo']['ySize'])
        for i, plane in enumerate(('ytexture', 'utexture', 'vtexture')):
            res = resources[i]
            expected = (width, height) if i == 0 else (width >> 1, height >> 1)
            if (int(res['width']), int(res['height'])) != expected or int(res['format']) != 1:
                return fail('incorrect size or format for ' + plane)
            pointer = (int(res.address) + int(res['imageDataOffset'])) & 0xffffffff
            if pointer != int(frame[plane]):
                return fail('incorrect pointer for ' + plane)
        state['frames'].add(int(frame['frameNumber']))
        if len(state['frames']) >= 3:
            if not state['placeholder'] or not state['skipped']:
                return fail('missing placeholder or skipped cutscene check')
            print('portal: PASS: cutscenes skipped, safe placeholder, three animated '
                  'Y/U/V frames bound (%dx%d)' % (width, height), flush=True)
            return True
        return False


class Perform(gdb.Breakpoint):
    def stop(self):
        if state['gate'] != int(gdb.parse_and_eval('this')):
            return False
        if not int(gdb.parse_and_eval('cue')) & 8:
            return False
        if not int(gdb.parse_and_eval('ActivePlayer.dispTextureSet')):
            gate = gdb.parse_and_eval('this')
            resources = gate['unk78']['mModel']['mModelData']['unkAC']['mResources']
            for i in range(3):
                res = resources[i]
                w, h = int(res['width']), int(res['height'])
                if (int(res['format']), w, h) != (0, 8, 8):
                    print('portal: placeholder %d: fmt=%d %dx%d' % (i, int(res['format']), w, h), flush=True)
                    return fail('placeholder expanded before a frame was decoded')
            state['placeholder'] = True
            return False
        Bound(gdb.newest_frame(), internal=True)
        return False


Load('TModelGate::loadAfter')
Play('THPPlayerPlay')
Perform('TModelGate::perform')
