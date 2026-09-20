import zipfile,sys,collections
Z=sys.argv[1]
CC0=31; AC0=7
# input-consuming RGB ccmux values (interpreter.cpp:306-317)
RGB_IN={3,10,14,4,5,12,13}   # PRIM, PRIM_ALPHA, PRIM_LOD_FRAC, SHADE, ENV, ENV_ALPHA, LOD_FRACTION
RGB_KNOWN={0,1,2,3,4,5,6,7,8,9,10,12,13,14,31}
def color_comb(a,b,c,d): return (a&0xf)|((b&0xf)<<4)|((c&0x1f)<<8)|((d&7)<<13)
def alpha_comb(a,b,c,d): return (a&7)|((b&7)<<3)|((c&7)<<6)|((d&7)<<9)
def C(w,p,n): return (w>>p)&((1<<n)-1)

def combine_mode(w0,w1):
    rgb0=color_comb(C(w0,20,4),C(w1,28,4),C(w0,15,5),C(w1,15,3))
    a0  =alpha_comb(C(w0,12,3),C(w1,12,3),C(w0,9,3),C(w1,9,3))
    rgb1=color_comb(C(w0,5,4), C(w1,24,4),C(w0,0,5), C(w1,6,3))
    a1  =alpha_comb(C(w1,21,3),C(w1,3,3), C(w1,18,3),C(w1,0,3))
    return rgb0|(a0<<16)|(rgb1<<28)|(a1<<44)

def analyze(cm,is2cyc):
    """Faithful port of Interpreter::GenerateCC slot allocation (interpreter.cpp:185-328)."""
    c=[[[0]*4 for _ in range(2)] for _ in range(2)]
    unsupported=False
    for i in range(2):
        if i==1 and not is2cyc: break
        s=i*28
        rgbA=(cm>>s)&0xf; rgbB=(cm>>(s+4))&0xf; rgbC=(cm>>(s+8))&0x1f; rgbD=(cm>>(s+13))&7
        aA=(cm>>(s+16))&7; aB=(cm>>(s+19))&7; aC=(cm>>(s+22))&7; aD=(cm>>(s+25))&7
        if rgbA>=8: rgbA=CC0
        if rgbB>=8: rgbB=CC0
        if rgbC>=16: rgbC=CC0
        if rgbD==7: rgbD=CC0
        if rgbA==rgbB or rgbC==CC0: rgbA=rgbB=rgbC=CC0
        if aA==aB or aC==AC0: aA=aB=aC=AC0
        if i==1:
            if rgbA!=0 and rgbB!=0 and rgbC!=0 and rgbD!=0:
                c[0][0]=[CC0]*4
            if rgbC!=7 and aA!=0 and aB!=0 and aD!=0:
                c[0][1]=[AC0]*4
        c[i][0]=[rgbA,rgbB,rgbC,rgbD]; c[i][1]=[aA,aB,aC,aD]
    if not is2cyc:
        c[1][0]=[CC0]*4; c[1][1]=[AC0]*4
    # RGB slot allocation
    seen={}; nxt=1
    for i in range(2):
        if i==1 and not is2cyc: break
        for j in range(4):
            v=c[i][0][j]
            if v in RGB_IN:
                if v not in seen: seen[v]=nxt; nxt+=1
            elif v not in RGB_KNOWN:
                unsupported=True
    nrgb=nxt-1
    # alpha slot allocation
    seenA={}; nxtA=1
    for i in range(2):
        for j in range(4):
            v=c[i][1][j]
            if v==0 and j==2: v=13                 # G_ACMUX_LOD_FRACTION at j==2
            elif v==6 and j!=2: v=None             # G_ACMUX_1
            elif v==0 and j!=2: v=None             # G_ACMUX_COMBINED
            elif v==7: v=None                      # G_ACMUX_0
            elif v in (1,2): v=None                # TEXEL0/1
            if v in (3,4,5,6,13):
                if v not in seenA: seenA[v]=nxtA; nxtA+=1
    nalpha=nxtA-1
    return max(nrgb,nalpha),unsupported

z=zipfile.ZipFile(Z)
hist1=collections.Counter(); hist2=collections.Counter()
uniq=set(); ncmd=0; nres=0; unsup=0
worst=[]
for name in z.namelist():
    d=z.read(name)
    if len(d)<72 or d[4:8]!=b'TLDO': continue
    nres+=1
    for o in range(64,len(d)-7,8):
        if d[o+3]!=0xFC: continue
        w0=int.from_bytes(d[o:o+4],'little'); w1=int.from_bytes(d[o+4:o+8],'little')
        if (w0>>24)!=0xFC: continue
        ncmd+=1
        cm=combine_mode(w0,w1); uniq.add(cm)
        n1,u1=analyze(cm,False); n2,u2=analyze(cm,True)
        hist1[n1]+=1; hist2[n2]+=1
        if u1 or u2: unsup+=1
        if n2>=5: worst.append((n2,name,hex(w0),hex(w1)))
print(f"resources scanned : {nres}")
print(f"SETCOMBINE cmds   : {ncmd}   unique combine_modes: {len(uniq)}")
print(f"unsupported ccmux : {unsup}")
print("\nnumInputs distribution (assuming 1-cycle):")
for k in sorted(hist1): print(f"   N={k}: {hist1[k]:7d}  ({100*hist1[k]/ncmd:5.2f}%)")
print("\nnumInputs distribution (assuming 2-cycle - upper bound):")
for k in sorted(hist2): print(f"   N={k}: {hist2[k]:7d}  ({100*hist2[k]/ncmd:5.2f}%)")
print(f"\ncommands with N>=5 (2-cycle): {len(worst)}")
for w in worst[:10]: print("   ",w)
