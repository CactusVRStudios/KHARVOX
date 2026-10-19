"""Generate the reviewed Steam/GOG address/signature profile from an offline audit."""
import argparse,json,struct,hashlib
from pathlib import Path
from audit_doom_builds import Image

def fnv(data):
    h=14695981039346656037
    for b in data:h=((h^b)*1099511628211)&0xffffffffffffffff
    return h

def main():
    p=argparse.ArgumentParser();p.add_argument('--steam',required=True);p.add_argument('--gog',required=True)
    p.add_argument('--audit',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();s,g=Image(a.steam),Image(a.gog);audit=json.loads(a.audit.read_text())
    if hashlib.sha256(s.data).hexdigest()!=audit['steam_sha256'] or hashlib.sha256(g.data).hexdigest()!=audit['gog_sha256']:raise ValueError('Audit inputs changed')
    result={int(r,16):[int(v[0],16),v[1],v[2],v[3]] for r,v in audit['mapped'].items() if r in audit['original_requested']}
    def string(img,ptr):return img.read(ptr-img.base,512).split(b'\0')[0]
    for r in (0x300f600,0x3035ad8,0x3035f10,0x307b400,0x30aa120,0x30aa1f8,0x30aacd8,
              0x31e3048,0x31e4638,0x3206230):
        field=r+24 if r==0x30aacd8 else r;names=struct.unpack('<3Q',s.read(field-24,24));hits=[]
        for target in g.find_all(s.read(field,8)):
            if target is None:continue
            pointers=struct.unpack('<3Q',g.read(target-24,24))
            if all(string(s,x)==string(g,y) for x,y in zip(names,pointers)):hits.append(target-(field-r))
        if len(hits)!=1:raise ValueError(f'Reflection record {r:x}: {hits}')
        result[r]=[hits[0],'reflection-type-name-offset-size',0,0]
    for r in (0x1508030,):
        hits=[x for x in g.find_all(s.read(r,8)) if x is not None and x<g.sections['.rdata'].VirtualAddress]
        if len(hits)!=1:raise ValueError('Ambiguous collectible flag getter')
        result[r]=[hits[0],'unique-leaf-body',0,0]
    for r in (0x27aa228,):
        hits=[x for x in g.find_all(string(s,s.base+r)+b'\0') if x is not None]
        if len(hits)!=1:raise ValueError('Ambiguous collectible enum label')
        result[r]=[hits[0],'unique-enum-label',0,0]
    for r in (0x3519988,0x3519998,0x35199a8):
        label=string(s,struct.unpack('<Q',s.read(r-8,8))[0]);hits=[]
        for nrva in g.find_all(label+b'\0'):
            if nrva is None:continue
            for record in g.find_all(struct.pack('<Q',g.base+nrva)):
                if record is not None and g.read(record+8,8)==s.read(r,8):hits.append(record+8)
        if len(hits)!=1:raise ValueError(f'Ambiguous collectible enum value {r:x}')
        result[r]=[hits[0],'enum-name-and-value',0,0]
    # The optional legacy two-view layout has stable record contents. Resolve
    # its labels and pointer records, not a section-wide guessed displacement.
    for r in (0x2850230,):
        value=s.read(r,64).split(b'\0')[0]+b'\0';hits=list(g.find_all(value))
        if len(hits)!=1:raise ValueError('Ambiguous stereo layout name')
        result[r]=[hits[0],'unique-layout-label',0,0]
    for r in (0x2850068,0x28500e0):
        name=string(s,struct.unpack('<Q',s.read(r,8))[0]);labels=list(g.find_all(name+b'\0'));hits=[]
        for label in labels:
            if label is not None:hits.extend(x for x in g.find_all(struct.pack('<Q',g.base+label)) if x is not None and g.read(x+8,32)==s.read(r+8,32))
        if len(hits)!=1:raise ValueError(f'Ambiguous layout pointer {r:x}')
        result[r]=[hits[0],'layout-pointer-and-record',0,0]
    r=0x2850090;target=result[0x2850068][0]+0x28
    if s.read(r+8,32)!=g.read(target+8,32):raise ValueError('Stereo layout fields differ')
    result[r]=[target,'verified-layout-record',0,0]
    # These literals are object offsets, signatures, limits or diagnostics,
    # not addresses used by the shipping AER/SFS engine adapters.
    excluded={0x1fffffe,0x300ad0,0x37e8930,0x3803af0,0x3c00000,0x3d00300,0x3e00100,0x3f00000,0x7230203,0x8000000,0xd8b0d3c}
    unresolved=set(int(x,16) for x in audit['original_requested'])-result.keys()-excluded
    if unresolved:raise ValueError(f'Unresolved requested addresses: {[hex(x) for x in sorted(unresolved)]}')
    for r in excluded:result.pop(r,None)
    patches={}
    for r,(target,proof,begin,size) in result.items():
        if r>=s.sections['.rdata'].VirtualAddress:continue
        # Exact profile-specific guard deltas. Do not relax masks in the
        # runtime: an unexpected byte in either input remains a mismatch.
        length=min(256,begin+size-r) if begin and size else 32
        for i,(old,new) in enumerate(zip(s.read(r,length),g.read(target,length))):
            if old!=new:
                # Identical COMDAT leaf functions can share one GOG address.
                # Keep each verified Steam input byte, with the same GOG byte.
                key=(target+i,old)
                if key in patches and patches[key]!=new:raise ValueError('Conflicting signature translation')
                patches[key]=new
    lines=['// Generated from the two SHA-256-pinned user-supplied executables.',
           '// Address pairs and guard deltas only; no executable payload.',
           'inline constexpr AddressPair addresses[]{']
    for r,(target,proof,_,__) in sorted(result.items()):lines.append(f'    {{0x{r:x},0x{target:x}}}, // {proof}')
    lines+=['};','inline constexpr GuardByte guardBytes[]{']
    for (r,old),new in sorted(patches.items()):lines.append(f'    {{0x{r:x},0x{old:02x},0x{new:02x}}},')
    lines+=['};','inline constexpr GuardHash guardHashes[]{']
    for r,size in [(0x295780,24),(0x295780,0x1bc),(0x1ade630,24),(0x1ade630,0x707),(0x1830b80,24),(0x1830b80,0x101)]:
        lines.append(f'    {{0x{r:x},0x{size:x},0x{fnv(s.read(r,size)):016x}ull,0x{fnv(g.read(result[r][0],size)):016x}ull}},')
    lines+=['};']
    for name,img in [('steam',s),('gog',g)]:
        sec=img.sections['.text'];lines.append(f'inline constexpr uint64_t {name}TextHash=0x{fnv(img.read(sec.VirtualAddress,sec.Misc_VirtualSize)):016x}ull;')
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text('\n'.join(lines)+'\n')
    evidence={hex(r):{'gog':hex(v[0]),'proof':v[1]} for r,v in sorted(result.items())}
    a.output.with_suffix('.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print(f'Generated {len(result)} address pairs, {len(patches)} signature-byte deltas and 6 code hashes')

if __name__=='__main__':main()
