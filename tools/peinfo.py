import sys, pefile
pe = pefile.PE(sys.argv[1])
print("ImageBase %x Entry %x" % (pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.AddressOfEntryPoint))
for s in pe.sections:
    print(s.Name.rstrip(b'\0'), hex(s.VirtualAddress), hex(s.Misc_VirtualSize), hex(s.SizeOfRawData), hex(s.Characteristics))
for e in getattr(pe,'DIRECTORY_ENTRY_IMPORT',[]):
    print(e.dll.decode(), ' '.join((i.name or b'#%d'%i.ordinal).decode() for i in e.imports))
