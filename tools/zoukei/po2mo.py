import ast, struct, sys
def parse(path):
    ents=[];cur={};key=None;fuzzy=False
    def flush():
        nonlocal cur,fuzzy
        if 'msgid' in cur and not fuzzy: ents.append(cur)
        cur={};fuzzy=False
    for line in open(path,encoding='utf-8'):
        line=line.strip()
        if not line:
            flush(); key=None; continue
        if line.startswith('#,') and 'fuzzy' in line: fuzzy=True; continue
        if line.startswith('#'): continue
        if line.startswith('"'):
            cur[key]+=ast.literal_eval(line); continue
        k,_,v=line.partition(' ')
        if k.startswith('msgid') or k.startswith('msgstr') or k=='msgctxt':
            if k=='msgid' and 'msgid' in cur: flush()
            key=k; cur[key]=ast.literal_eval(v)
    flush(); return ents
def build(ents):
    m={}
    for e in ents:
        mid=e['msgid']
        if 'msgid_plural' in e:
            mid+='\0'+e['msgid_plural']
            strs=[e[k] for k in sorted(k for k in e if k.startswith('msgstr['))]
            ms='\0'.join(strs)
        else: ms=e.get('msgstr','')
        if not ms.replace('\0','') and mid: continue
        if 'msgctxt' in e: mid=e['msgctxt']+'\x04'+mid
        m[mid.encode()]=ms.encode()
    keys=sorted(m); ids=b'';strs=b'';offs=[]
    for k in keys:
        offs.append((len(ids),len(k),len(strs),len(m[k]))); ids+=k+b'\0'; strs+=m[k]+b'\0'
    n=len(keys); ks=7*4+16*n; vs=ks+len(ids)
    ko=[];vo=[]
    for a,b,c,d in offs: ko+=[b,a+ks]; vo+=[d,c+vs]
    return struct.pack('Iiiiiii',0x950412de,0,n,28,28+8*n,0,0)+struct.pack(f'{len(ko)}i',*ko)+struct.pack(f'{len(vo)}i',*vo)+ids+strs
open(sys.argv[2],'wb').write(build(parse(sys.argv[1])))
