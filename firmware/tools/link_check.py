def check_sections(run, elf):
    # Stock startup does not initialise RAM for the added code.
    for line in run("objdump", "-h", elf).splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[0].isdigit():
            name, size = fields[1], int(fields[2], 16)
            if size and name not in (".text", ".comment", ".ARM.attributes"):
                raise RuntimeError(
                    f"{elf}: unsupported section {name}. Use explicitly reserved RAM."
                )
