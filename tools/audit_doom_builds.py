"""Offline DOOM build comparison. Reads supplied EXEs; never executes game code.

Dependencies: pefile, capstone. No copyrighted executable bytes are exported.
"""
import argparse, bisect, collections, hashlib, json, re, struct
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_JUMP, CS_GRP_CALL
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

class Image:
    def __init__(self, path):
        self.path=Path(path);self.data=self.path.read_bytes()
        self.pe=pefile.PE(data=self.data,fast_load=True)
        self.base=self.pe.OPTIONAL_HEADER.ImageBase
        self.sections={s.Name.rstrip(b'\0').decode():s for s in self.pe.sections}
        section=self.sections['.pdata'];raw=self.read(section.VirtualAddress,section.Misc_VirtualSize)
        self.functions=[x for x in struct.iter_unpack('<III',raw) if x[0] and x[1]>x[0]]
        self.starts=[x[0] for x in self.functions];self.by_start={x[0]:x for x in self.functions}
        self.md=Cs(CS_ARCH_X86,CS_MODE_64);self.md.detail=True
        self.cache={}
    def rva_at_file(self,offset):
        for s in self.pe.sections:
            if s.PointerToRawData<=offset<s.PointerToRawData+s.SizeOfRawData:return offset-s.PointerToRawData+s.VirtualAddress
        return None
    def find_all(self,value):
        start=0
        while True:
            n=self.data.find(value,start)
            if n<0:return
            yield self.rva_at_file(n);start=n+1
    def vtable_type(self,rva):
        try:
            col=struct.unpack('<Q',self.read(rva-8,8))[0]-self.base
            sig,offset,cd,td,ch,selfrva=struct.unpack('<6I',self.read(col,24))
            if sig!=1 or selfrva!=col:return None
            name=self.read(td+16,256).split(b'\0')[0]
            return (name,offset,cd) if name.startswith(b'.?AV') else None
        except (struct.error,ValueError):return None
    def find_vtable(self,identity):
        name,offset,cd=identity;result=[]
        for nrva in self.find_all(name+b'\0'):
            if nrva is None:continue
            td=nrva-16
            for field in self.find_all(struct.pack('<I',td)):
                if field is None:continue
                col=field-12
                if self.read(col,12)!=struct.pack('<III',1,offset,cd) or self.read(col+20,4)!=struct.pack('<I',col):continue
                for ptr in self.find_all(struct.pack('<Q',self.base+col)):
                    if ptr is not None and self.vtable_type(ptr+8)==identity:result.append(ptr+8)
        return result
    def read(self,rva,n):
        for s in self.pe.sections:
            d=rva-s.VirtualAddress
            if 0<=d<s.SizeOfRawData:return self.data[s.PointerToRawData+d:s.PointerToRawData+d+n]
        return b''
    def containing(self,rva):
        i=bisect.bisect_right(self.starts,rva)-1
        return self.functions[i] if i>=0 and rva<self.functions[i][1] else None
    def normalized(self,f):
        if f[0] in self.cache:return self.cache[f[0]]
        raw=bytearray(self.read(f[0],f[1]-f[0]));refs=[];end=f[0];table_begin=f[1]
        for ins in self.md.disasm(bytes(raw),f[0]):
            if ins.address>=table_begin:break
            end=ins.address+ins.size;offset=ins.address-f[0]
            for op in ins.operands:
                if op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP:
                    refs.append((offset,'rip',end+op.mem.disp))
                    raw[offset+ins.disp_offset:offset+ins.disp_offset+ins.disp_size]=b'\0'*ins.disp_size
                elif op.type==X86_OP_MEM and f[0]<=op.mem.disp<f[1]:
                    table_begin=min(table_begin,op.mem.disp)
                    refs.append((offset,'table',op.mem.disp))
                    raw[offset+ins.disp_offset:offset+ins.disp_offset+ins.disp_size]=b'\0'*ins.disp_size
                elif op.type==X86_OP_MEM and ins.group(CS_GRP_CALL) and ins.disp_size:
                    refs.append((offset,'vslot',op.mem.disp))
                    raw[offset+ins.disp_offset:offset+ins.disp_offset+ins.disp_size]=b'\0'*ins.disp_size
                elif op.type==X86_OP_IMM and (ins.group(CS_GRP_JUMP) or ins.group(CS_GRP_CALL)):
                    refs.append((offset,'branch',op.imm))
                    raw[offset+ins.imm_offset:offset+ins.imm_offset+ins.imm_size]=b'\0'*ins.imm_size
                elif op.type==X86_OP_IMM and self.base<=op.imm<self.base+self.pe.OPTIONAL_HEADER.SizeOfImage:
                    refs.append((offset,'absolute',op.imm-self.base))
                    raw[offset+ins.imm_offset:offset+ins.imm_offset+ins.imm_size]=b'\0'*ins.imm_size
        # RVA jump-table entries embedded in a function's unwind range.
        original=self.read(f[0],f[1]-f[0])
        for i in range(table_begin-f[0],len(original)-3):
            value=struct.unpack_from('<I',original,i)[0]
            if f[0]<=value<f[1]:raw[i:i+4]=struct.pack('<I',value-f[0])
        result=(bytes(raw),refs,end==f[1]);self.cache[f[0]]=result;return result

def compare(steam,gog,requested):
    indices=[]
    for img in (steam,gog):
        index=collections.defaultdict(list)
        for f in img.functions:
            index[hashlib.sha256(img.read(f[0],f[1]-f[0])).digest()].append(f[0])
        indices.append(index)
    anchors=sorted((a[0],indices[1][h][0]) for h,a in indices[0].items()
                   if len(a)==1 and len(indices[1].get(h,[]))==1)
    # Reject incidental identical functions which moved to unrelated positions.
    anchors=[(a,b) for a,b in anchors if abs(a-b)<0x10000]
    anchor_starts=[a for a,b in anchors];mapped={};references=collections.defaultdict(set);unmatched=[]
    tables={};table_functions={}
    for rva in sorted(requested):
        identity=steam.vtable_type(rva)
        if identity:
            hits=gog.find_vtable(identity)
            if len(hits)==1:
                tables[rva]=(hits[0],identity[0].decode())
                for slot in range(220):
                    a=steam.read(rva+slot*8,8);b=gog.read(hits[0]+slot*8,8)
                    if len(a)!=8 or len(b)!=8:break
                    a=struct.unpack('<Q',a)[0]-steam.base;b=struct.unpack('<Q',b)[0]-gog.base
                    if not 0x1000<=a<steam.sections['.text'].Misc_VirtualSize or not 0x1000<=b<gog.sections['.text'].Misc_VirtualSize:break
                    table_functions.setdefault(a,set()).add(b)
    funcs={steam.containing(rva) for rva in requested};funcs.discard(None)
    for f in sorted(funcs):
        i=bisect.bisect_left(anchor_starts,f[0]);candidates=set()
        for a,b in anchors[max(0,i-8):i+8]:
            g=gog.by_start.get(f[0]+b-a)
            if g and g[1]-g[0]==f[1]-f[0]:candidates.add(g)
        for r in table_functions.get(f[0],[]):
            g=gog.by_start.get(r)
            if g and g[1]-g[0]==f[1]-f[0]:candidates.add(g)
        norm,refs,complete=steam.normalized(f)
        matches=[g for g in candidates if gog.normalized(g)[0]==norm
                 and [r[:2] for r in refs]==[r[:2] for r in gog.normalized(g)[1]]
                 and all(b[2]-a[2] in (0,8) for a,b in zip(refs,gog.normalized(g)[1]) if a[1]=='vslot')]
        if len(matches)!=1:
            unmatched.append({'steam':hex(f[0]),'size':f[1]-f[0],'candidates':[hex(g[0]) for g in candidates],'matches':len(matches)})
            continue
        g=matches[0];mapped[f[0]]=(f,g)
        for a,b in zip(refs,gog.normalized(g)[1]):
            if a[:2]!=b[:2]:raise RuntimeError('Reference instruction mismatch')
            if a[1]!='vslot':references[a[2]].add(b[2])
    result={}
    for rva in sorted(requested):
        f=steam.containing(rva)
        if f and f[0] in mapped:
            a,b=mapped[f[0]];result[rva]=(b[0]+rva-a[0],'normalized-function',a[0],a[1]-a[0])
        elif len(references[rva])==1:result[rva]=(next(iter(references[rva])),'instruction-reference',0,0)
        elif rva in tables:result[rva]=(tables[rva][0],'RTTI:'+tables[rva][1],0,0)
        elif len(table_functions.get(rva,set()))==1:
            target=next(iter(table_functions[rva]));size=0
            for ins in steam.md.disasm(steam.read(rva,64),rva):
                size=ins.address+ins.size-rva
                if ins.mnemonic in ('jmp','ret'):break
            if size and steam.normalized((rva,rva+size,0))[0]==gog.normalized((target,target+size,0))[0]:
                result[rva]=(target,'RTTI-leaf-body',rva,size)
    return result,mapped,references,unmatched,len(anchors)

def main():
    p=argparse.ArgumentParser();p.add_argument('--steam',required=True);p.add_argument('--gog',required=True)
    p.add_argument('--root',type=Path,default=Path('.'));p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();s,g=Image(args.steam),Image(args.gog)
    requested=set()
    for path in (args.root/'src').rglob('*'):
        if path.suffix not in ('.cpp','.h','.inc') or 'upstream' in path.parts or path.name.startswith('DoomBuild'):continue
        text=path.read_text(errors='replace')
        text=re.sub(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'""", '', text, flags=re.S)
        requested.update(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]+)',text) if 0x100000<=int(x,16)<s.pe.OPTIONAL_HEADER.SizeOfImage)
    # Registration/accessor code gives evidence for otherwise zero-initialized
    # globals. Locate instruction references; do not infer a blanket delta.
    original=set(requested);sec=s.sections['.text'];code=s.read(sec.VirtualAddress,sec.Misc_VirtualSize)
    wanted=sorted(r for r in requested if r>=s.sections['.rdata'].VirtualAddress)
    for match in re.finditer(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]....',code,re.S):
        at=sec.VirtualAddress+match.start();dest=at+7+struct.unpack_from('<i',match[0],3)[0]
        i=bisect.bisect_left(wanted,dest)
        if any(abs(r-dest)<=0x60 for r in wanted[max(0,i-1):i+1]):requested.add(at)
    result,mapped,refs,unmatched,anchors=compare(s,g,requested)
    # A field in the same CVar/reflection record is verified relative to its
    # directly referenced object, with consistent mappings from nearby refs.
    for rva in sorted(original-result.keys()):
        near=[(a,next(iter(bs))) for a,bs in refs.items() if len(bs)==1 and a<=rva<=a+0x60 and a>=s.sections['.rdata'].VirtualAddress]
        deltas={b-a for a,b in near}
        if len(deltas)==1:result[rva]=(rva+next(iter(deltas)),'referenced-object-field',0,0)
    payload={'steam_sha256':hashlib.sha256(s.data).hexdigest(),'gog_sha256':hashlib.sha256(g.data).hexdigest(),
             'original_requested':[hex(x) for x in sorted(original)],
             'anchors':anchors,'requested':len(requested),'mapped':{hex(k):[hex(v[0]),*v[1:]] for k,v in result.items()},
             'references':{hex(k):[hex(v) for v in values] for k,values in refs.items()},
             'unresolved':[hex(r) for r in sorted(requested-result.keys())],'unmatched_functions':unmatched}
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(payload,indent=2)+'\n')
    print(f'anchors={anchors} requested={len(requested)} mapped={len(result)} functions={len(mapped)} unresolved={len(payload["unresolved"])}')
    print('Original unresolved:',[hex(x) for x in sorted(original-result.keys())])

if __name__=='__main__':main()
