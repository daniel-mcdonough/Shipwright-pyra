# How many of the game's colour combiners can skip the shader's WRAP()?
#
#   python3 wrapscan.py /path/to/oot.o2r
#
# The wrap models the N64 combiner's modulo behaviour. It is only reachable when a cycle emits the
# full (A-B)*C+D form; the three reduced forms Fast3D produces cannot leave [0,1]. This scans every
# G_SETCOMBINE in an archive, reproduces Interpreter::GenerateCC and gfx_cc_get_features, and
# applies the same test CombinerCycleInRangeFor applies in gfx_opengl.cpp.
#
# What it is not: a fragment-weighted measure. A combiner set once for a full-screen quad counts
# the same as one set for four pixels of HUD, and the display list does not say which. Read the
# unique-variant column as "how much of the shader set this touches" and the command column as a
# rough proxy for how often it is selected. Only the device says what it is worth.
#
# Modelled on ccscan.py, which decodes the same commands for a different question.
import zipfile, sys, collections

CC0 = 31  # G_CCMUX_0
AC0 = 7   # G_ACMUX_0

# SHADER_* enum, interpreter.h:43
(SHADER_0, SHADER_INPUT_1, SHADER_INPUT_2, SHADER_INPUT_3, SHADER_INPUT_4, SHADER_INPUT_5,
 SHADER_INPUT_6, SHADER_INPUT_7, SHADER_TEXEL0, SHADER_TEXEL0A, SHADER_TEXEL1, SHADER_TEXEL1A,
 SHADER_1, SHADER_COMBINED, SHADER_NOISE) = range(15)

CC_INPUTS = {3, 10, 14, 4, 5, 12, 13}  # PRIM, PRIM_ALPHA, PRIM_LOD_FRAC, SHADE, ENV, ENV_ALPHA, LOD_FRACTION
AC_INPUTS = {3, 4, 5}                  # PRIMITIVE, SHADE, ENVIRONMENT


def color_comb(a, b, c, d):
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13)


def alpha_comb(a, b, c, d):
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9)


def C(w, p, n):
    return (w >> p) & ((1 << n) - 1)


def combine_mode(w0, w1):
    rgb0 = color_comb(C(w0, 20, 4), C(w1, 28, 4), C(w0, 15, 5), C(w1, 15, 3))
    a0 = alpha_comb(C(w0, 12, 3), C(w1, 12, 3), C(w0, 9, 3), C(w1, 9, 3))
    rgb1 = color_comb(C(w0, 5, 4), C(w1, 24, 4), C(w0, 0, 5), C(w1, 6, 3))
    a1 = alpha_comb(C(w1, 21, 3), C(w1, 3, 3), C(w1, 18, 3), C(w1, 0, 3))
    return rgb0 | (a0 << 16) | (rgb1 << 28) | (a1 << 44)


def generate_cc(cm, is2cyc):
    """Interpreter::GenerateCC, interpreter.cpp:199. Returns (c[2][2][4] as SHADER_*, shader_id0)."""
    raw = [[[0] * 4 for _ in range(2)] for _ in range(2)]
    for i in range(2):
        if i == 1 and not is2cyc:
            break
        s = i * 28
        rgbA, rgbB, rgbC, rgbD = (cm >> s) & 0xf, (cm >> (s + 4)) & 0xf, (cm >> (s + 8)) & 0x1f, (cm >> (s + 13)) & 7
        aA, aB, aC, aD = (cm >> (s + 16)) & 7, (cm >> (s + 19)) & 7, (cm >> (s + 22)) & 7, (cm >> (s + 25)) & 7
        if rgbA >= 8: rgbA = CC0
        if rgbB >= 8: rgbB = CC0
        if rgbC >= 16: rgbC = CC0
        if rgbD == 7: rgbD = CC0
        if rgbA == rgbB or rgbC == CC0: rgbA = rgbB = rgbC = CC0
        if aA == aB or aC == AC0: aA = aB = aC = AC0
        if i == 1:
            # 0 is G_CCMUX_COMBINED; 7 is G_CCMUX_COMBINED_ALPHA.
            if rgbA != 0 and rgbB != 0 and rgbC != 0 and rgbD != 0:
                raw[0][0] = [CC0] * 4
            if rgbC != 7 and aA != 0 and aB != 0 and aD != 0:
                raw[0][1] = [AC0] * 4
        raw[i][0] = [rgbA, rgbB, rgbC, rgbD]
        raw[i][1] = [aA, aB, aC, aD]
    if not is2cyc:
        raw[1][0] = [CC0] * 4
        raw[1][1] = [AC0] * 4

    c = [[[0] * 4 for _ in range(2)] for _ in range(2)]
    sid0 = 0

    slot, nxt = {}, SHADER_INPUT_1
    for i in range(2):
        if i == 1 and not is2cyc:
            break
        for j in range(4):
            v = raw[i][0][j]
            if v == CC0: val = SHADER_0
            elif v == 6: val = SHADER_1              # G_CCMUX_1
            elif v == 1: val = SHADER_TEXEL0
            elif v == 2: val = SHADER_TEXEL1
            elif v == 8: val = SHADER_TEXEL0A
            elif v == 9: val = SHADER_TEXEL1A
            elif v == 7: val = SHADER_NOISE
            elif v == 0: val = SHADER_COMBINED
            elif v in CC_INPUTS:
                if v not in slot:
                    slot[v] = nxt
                    nxt += 1
                val = slot[v]
            else:
                val = SHADER_0                        # unsupported ccmux; GenerateCC leaves val 0
            c[i][0][j] = val
            sid0 |= val << (i * 32 + j * 4)

    slotA, nxtA = {}, SHADER_INPUT_1
    for i in range(2):
        for j in range(4):
            v = raw[i][1][j]
            val = SHADER_0
            if v == AC0: val = SHADER_0
            elif v == 1: val = SHADER_TEXEL0
            elif v == 2: val = SHADER_TEXEL1
            elif v == 0:                              # COMBINED, or LOD_FRACTION in slot C
                if j != 2:
                    val = SHADER_COMBINED
                else:
                    v = 13                            # becomes an input
            elif v == 6:                              # 1, or PRIM_LOD_FRAC in slot C
                if j != 2:
                    val = SHADER_1
            if val == SHADER_0 and v in (AC_INPUTS | {6, 13}) and v != AC0:
                if v not in slotA:
                    slotA[v] = nxtA
                    nxtA += 1
                val = slotA[v]
            c[i][1][j] = val
            sid0 |= val << (i * 32 + 16 + j * 4)

    return c, sid0


def in_range(item, prev):
    return item != SHADER_COMBINED or prev


def channel_in_range(c, do_single, do_multiply, do_mix, cyc, i, prev):
    if do_single[cyc][i]:
        return in_range(c[cyc][i][3], prev)
    if do_multiply[cyc][i]:
        return in_range(c[cyc][i][0], prev) and in_range(c[cyc][i][2], prev)
    if do_mix[cyc][i]:
        return all(in_range(c[cyc][i][k], prev) for k in (0, 1, 2))
    return False


def analyse(cm, is2cyc, opt_alpha):
    c, sid0 = generate_cc(cm, is2cyc)
    do_single = [[c[x][i][2] == SHADER_0 for i in range(2)] for x in range(2)]
    do_multiply = [[c[x][i][1] == SHADER_0 and c[x][i][3] == SHADER_0 for i in range(2)] for x in range(2)]
    do_mix = [[c[x][i][1] == c[x][i][3] for i in range(2)] for x in range(2)]
    same = [((sid0 >> (x * 32)) & 0xffff) == ((sid0 >> (x * 32 + 16)) & 0xffff) for x in range(2)]

    def cycle(x, prev):
        if not channel_in_range(c, do_single, do_multiply, do_mix, x, 0, prev):
            return False
        if opt_alpha and not same[x]:
            return channel_in_range(c, do_single, do_multiply, do_mix, x, 1, prev)
        return True

    cyc0 = cycle(0, False)
    last = cycle(1, cyc0) if is2cyc else cyc0
    return sid0, cyc0, last


def main(path):
    z = zipfile.ZipFile(path)
    cmds = collections.Counter()
    for name in z.namelist():
        d = z.read(name)
        if len(d) < 72 or d[4:8] != b'TLDO':
            continue
        for o in range(64, len(d) - 7, 8):
            if d[o + 3] != 0xFC:
                continue
            w0 = int.from_bytes(d[o:o + 4], 'little')
            w1 = int.from_bytes(d[o + 4:o + 8], 'little')
            if (w0 >> 24) != 0xFC:
                continue
            cmds[combine_mode(w0, w1)] += 1

    total_cmds = sum(cmds.values())
    print(f"{path}\nG_SETCOMBINE commands: {total_cmds}   unique combine_modes: {len(cmds)}\n")
    print(f"{'assumption':<28}{'variants':>18}{'commands':>18}")
    for is2cyc in (False, True):
        for opt_alpha in (True, False):
            variants, elided_v, elided_c = {}, 0, 0
            for cm, n in cmds.items():
                sid0, _cyc0, last = analyse(cm, is2cyc, opt_alpha)
                variants.setdefault(sid0, last)
                if last:
                    elided_c += n
            elided_v = sum(1 for v in variants.values() if v)
            label = f"{'2-cycle' if is2cyc else '1-cycle'}, alpha {'on' if opt_alpha else 'off'}"
            print(f"{label:<28}{elided_v:>7}/{len(variants):<10}{100 * elided_v / len(variants):>6.1f}%"
                  f"{elided_c:>9}/{total_cmds:<8}{100 * elided_c / total_cmds:>6.1f}%")


if __name__ == "__main__":
    main(sys.argv[1])
