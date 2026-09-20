# minimal Lua 5.1-float (numbers=4B float) undump -> readable listing + strings
import struct, sys
def u32(d,p): return struct.unpack_from('<I',d,p)[0],p+4
def byte(d,p): return d[p],p+1
def string(d,p):
    n,p=u32(d,p)
    if n==0: return None,p
    s=d[p:p+n-1]; return s.decode('utf-8','replace'),p+n
def func(d,p,depth=0,path='main'):
    out=[]; ind='  '*depth
    out.append(f"{ind}== function {path} ==")
    sname,p=string(d,p)
    out.append(f"{ind}source: {sname!r}")
    srcline,p=u32(d,p)
    elastline,p=u32(d,p)
    nup,p=byte(d,p); npar,p=byte(d,p); var,p=byte(d,p); stack,p=byte(d,p)
    out.append(f"{ind}linedefined={srcline}-{elastline} params={npar} vararg={var} stack={stack} upvalues={nup}")
    ncode,p=u32(d,p)
    code=[u32(d,q)[0] for q in range(p,p+4*ncode,4)]; p+=4*ncode
    OP=['MOVE','LOADK','LOADBOOL','LOADNIL','GETUPVAL','GETGLOBAL','GETTABLE','SETGLOBAL','SETUPVAL','SETTABLE','NEWTABLE','SELF','ADD','SUB','MUL','DIV','MOD','POW','UNM','NOT','LEN','CONCAT','JMP','EQ','LT','LE','TEST','TESTSET','CALL','TAILCALL','RETURN','FORLOOP','FORPREP','TFORLOOP','SETLIST','CLOSE','CLOSURE','VARARG']
    out.append(f"{ind}code[{ncode}]:")
    for i,c in enumerate(code):
        op=c&63; A=(c>>6)&255; C=(c>>14)&511; B=(c>>23)&511; Bx=(c>>14)&262143; sBx=Bx-131071
        nm=OP[op] if op<len(OP) else f'OP{op}'
        out.append(f"{ind}  [{i+1:4}] {nm:9} A={A:3} B={B:3} C={C:3} Bx={Bx:6} sBx={sBx:6}")
    nk,p=u32(d,p)
    out.append(f"{ind}consts[{nk}]:")
    for i in range(nk):
        t,p=byte(d,p)
        if t==0: out.append(f"{ind}  [{i}] NIL");
        elif t==1: v,p=byte(d,p); out.append(f"{ind}  [{i}] BOOL {bool(v)}")
        elif t==3:
            v=struct.unpack_from('<f',d,p)[0]; p+=4; out.append(f"{ind}  [{i}] NUM {v!r}")
        elif t==4:
            s,p=string(d,p); out.append(f"{ind}  [{i}] STR {s!r}")
        else: out.append(f"{ind}  [{i}] TYPE{t}?"); break
    np,p=u32(d,p)
    out.append(f"{ind}protos[{np}]:")
    for i in range(np):
        # NOTE: func() itself reads the child source string first; the
        # pre-read here only peeks for the display name without consuming.
        peek, _ = string(d,p)
        f,p,sub=func(d,p,depth+1,f"{path}/{peek or '?'}:{i}")
        out.append(f"{ind}  [{i}] proto {peek!r}:")
        out.extend(sub)
    sl,p=u32(d,p); el,p=u32(d,p)
    out.append(f"{ind}srcline: {sl}-{el}")
    nl,p=u32(d,p)
    out.append(f"{ind}lineinfo[{nl}]")
    p+=4*nl
    # Rovio strips debug: locvar/upvalue tables are absent everywhere
    # (chunk ends after lineinfo, even for nested funcs — verified: exact
    # end-of-buffer match on full recursive skip). Never parse them.
    out.append(f"{ind}locvars[stripped]")
    out.append(f"{ind}upvalues[stripped]")
    return path,p,out
def dump(path):
    d=open(path,'rb').read()
    assert d[:4]==b'\x1bLua' and d[4]==0x51, 'not AB luac'
    assert d[5:12]==bytes([0,1,4,4,4,4,0]), 'unexpected AB header '+d[5:12].hex()
    f,p,out=func(d,12)
    assert p==len(d), f'trailing {len(d)-p} bytes at {p}/{len(d)}'
    return '\n'.join(out)
if __name__=='__main__':
    print(dump(sys.argv[1]))
