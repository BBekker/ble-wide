# SPDX-License-Identifier: MIT
def pairing(reference, candidate):
    """Identity includes payload and CRC; allow <=4 2-Msps timing samples."""
    index={}
    for i,p in enumerate(candidate):
        key=tuple(p[k] for k in ('channel','access_address','pdu','crc'))
        index.setdefault(key,[]).append((i,p['sample']))
    used=set(); pairs=[]; missing=[]
    for p in reference:
        key=tuple(p[k] for k in ('channel','access_address','pdu','crc'))
        possible=[(abs(s-p['sample']),i) for i,s in index.get(key,[]) if i not in used and abs(s-p['sample'])<=4]
        if possible:
            _,i=min(possible);used.add(i);pairs.append((p,candidate[i]))
        else:missing.append(p)
    return pairs,missing,[p for i,p in enumerate(candidate) if i not in used]
