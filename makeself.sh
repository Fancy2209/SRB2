ppu-strip build/bin/srb2_PS3.elf -o build/srb2_PS3_IGNORE.elf
sprxlinker build/srb2_PS3_IGNORE.elf
make_self build/srb2_PS3_IGNORE.elf build/bin/srb2_PS3.self
rm build/srb2_PS3_IGNORE.elf
