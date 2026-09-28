import gdb,re
names=set()
for line in gdb.execute('info types .', to_string=True).splitlines():
    m=re.match(r'^\s*(?:\d+:\s*)?(?!typedef)([A-Za-z_][\w:<>, *]*?);?\s*$', line.strip())
    if m and not line.startswith('File') and not line.startswith('All'): names.add(m.group(1).strip())
open(out_file,'w').write('\n'.join(sorted(names)))
