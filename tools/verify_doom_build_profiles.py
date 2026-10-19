"""Read-only binary/profile validation. Never loads or executes the supplied EXEs.

Requires pefile and capstone, as used by audit_doom_builds.py.
"""
import argparse, hashlib, json, re, struct
from pathlib import Path
from audit_doom_builds import Image

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--steam',required=True);p.add_argument('--gog',required=True)
    p.add_argument('--root',type=Path,default=Path('.'))
    p.add_argument('--report',type=Path,required=True)
    a=p.parse_args();s,g=Image(a.steam),Image(a.gog)
    assert hashlib.sha256(s.data).hexdigest()=='3d8b4393649f80bdae85c19ae2c985c45ec9ac54f74ab04a18816e30ead63c21'
    assert hashlib.sha256(g.data).hexdigest()=='a32df8ffa042090f14fe0a200f1c5d7dddf9c947fac223916c252f826f1ecf11'
    profile=json.loads((a.root/'src/common/DoomBuildData.json').read_text())
    mapping={int(k,16):int(v['gog'],16) for k,v in profile.items()}
    inc=(a.root/'src/common/DoomBuildData.inc').read_text().split('guardBytes[]{')[1].split('};')[0]
    patches={(int(x,16),int(y,16)):int(z,16) for x,y,z in re.findall(r'\{0x([0-9a-f]+),0x([0-9a-f]+),0x([0-9a-f]+)\}',inc)}
    failures=[];guards=[];tables=[];source_calls=0
    for source in (a.root/'src').rglob('*'):
        if source.suffix not in ('.h','.cpp','.inc') or 'upstream' in source.parts or source.name.startswith('DoomBuild'):continue
        text=source.read_text(errors='replace')
        for v in re.findall(r'game::rva\((0x[0-9a-fA-F]+)\)',text):
            source_calls+=1
            if int(v,16) not in mapping:failures.append(f'{source}: unmapped {v}')
        # Enumerate byte-array guards directly from production sources. Each
        # known guard must still match at its translated GOG address.
        text=re.sub(r'//[^\n]*|/\*.*?\*/','',text,flags=re.S)
        pattern=r'(?:unsigned char|uint8_t|std::array<\s*(?:unsigned char|uint8_t)\s*,\s*\d+\s*>)\s+(\w+)\s*(?:\[\s*\])?\s*(?:=\s*)?\{([^{}]+)\}'
        for match in re.finditer(pattern,text):
            name,values=match.groups()
            if not re.search(r'signature|code|flagRead|prefix|getter|globalRead',name,re.I):continue
            values=values.replace('\n','').strip().rstrip(',')
            if not re.fullmatch(r'[\s,0-9a-fA-Fx]+',values):continue
            try:guard=bytes(int(x.strip(),0) for x in values.split(','))
            except ValueError:continue
            if len(guard)<4:continue
            hits=[r for r in s.find_all(guard) if r in mapping]
            if not hits:
                # Main camera signatures are found by a unique text scan,
                # without a fixed RVA. Require the same unique GOG pattern.
                sh=[r for r in s.find_all(guard) if r and r<s.sections['.rdata'].VirtualAddress]
                gh=[r for r in g.find_all(guard) if r and r<g.sections['.rdata'].VirtualAddress]
                if len(sh)==1 and len(gh)==1:
                    guards.append(dict(source=str(source),name=name,steam=hex(sh[0]),gog=hex(gh[0]),scan=True))
                continue
            for r in hits:
                target=mapping[r]
                expected=bytes(patches.get((target+i,b),b) for i,b in enumerate(guard))
                if g.read(target,len(expected))!=expected:failures.append(f'{source}: {name} at {r:x} signature mismatch')
                guards.append(dict(source=str(source),name=name,steam=hex(r),gog=hex(target),length=len(guard)))
    # Validate identity and unchanged slot indices for mapped HUD vtables.
    # Check every slot whose Steam target is one of our reviewed functions.
    for r,target in mapping.items():
        identity=s.vtable_type(r)
        if not identity:continue
        if g.vtable_type(target)!=identity:failures.append(f'Vtable identity {r:x}')
        verified=[]
        for slot in range(256):
            x=struct.unpack('<Q',s.read(r+slot*8,8))[0]-s.base
            y=struct.unpack('<Q',g.read(target+slot*8,8))[0]-g.base
            if not (0x1000<=x<s.sections['.rdata'].VirtualAddress):break
            if x in mapping:
                if mapping[x]!=y:failures.append(f'Vtable {r:x} slot {slot}: {x:x} -> {y:x}, expected {mapping[x]:x}')
                verified.append(slot)
        tables.append(dict(steam=hex(r),gog=hex(target),identity=str(identity),verified_slots=verified))
    # Player virtual calls used by the camera adapter and inventory query.
    st=s.find_vtable((b'.?AVidPlayer@@',0,0));gt=g.find_vtable((b'.?AVidPlayer@@',0,0))
    assert len(st)==len(gt)==1
    for slot in (0x368,0x370,0x660):
        x=struct.unpack('<Q',s.read(st[0]+slot,8))[0]-s.base
        y=struct.unpack('<Q',g.read(gt[0]+slot,8))[0]-g.base
        # Normalize RIP operands while retaining object offsets and opcodes.
        def leaf(im,r):
            code=bytearray();
            for ins in im.md.disasm(im.read(r,128),r):
                raw=bytearray(ins.bytes)
                if 'rip' in ins.op_str:raw[ins.disp_offset:ins.disp_offset+ins.disp_size]=b'\0'*ins.disp_size
                code.extend(raw)
                if ins.mnemonic=='ret':return bytes(code)
            raise AssertionError('Not a verified leaf')
        if leaf(s,x)!=leaf(g,y):failures.append(f'idPlayer slot {slot:x} semantics differ')
    report=dict(addresses=len(mapping),mapped_source_calls=source_calls,guards=guards,vtables=tables,
                player_slots=['0x368','0x370','0x660'],failures=failures,
                scope='Offline binary validation only; GOG gameplay and hardware not tested')
    a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f'{len(mapping)} addresses; {source_calls} source calls; {len(guards)} guards; {len(tables)} vtables; {len(failures)} failures')
    for failure in failures:print(failure)
    if failures:raise SystemExit(1)

if __name__=='__main__':main()
