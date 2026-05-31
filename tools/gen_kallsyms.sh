#!/bin/sh
#
# tools/gen_kallsyms.sh NM ELF > out.S
#
# Emit an assembly file that populates the .kallsyms section with
# every text symbol in ELF, sorted by address.  See
# include/kappara/kallsyms.h for the layout.

set -e

NM="$1"
ELF="$2"
if [ -z "$NM" ] || [ -z "$ELF" ]; then
	echo "usage: $0 NM ELF" >&2
	exit 1
fi

"$NM" --numeric-sort "$ELF" |
awk '
	BEGIN { n = 0 }	# force numeric, not empty-string, subscripts

	# Keep text symbols only (T = global, t = local, W/w = weak).
	# Drop linker-emitted labels we do not care about ("$x", "$d").
	$2 == "T" || $2 == "t" || $2 == "W" || $2 == "w" {
		name = $3
		if (substr(name, 1, 1) == "$") next
		addrs[n] = $1
		names[n] = name
		n++
	}
	END {
		print ".section .kallsyms, \"a\""
		print ".align 8"
		print ".globl __kallsyms_magic"
		print "__kallsyms_magic:"
		print "\t.4byte 0x4d59534b"   # "KSYM" little-endian
		print ".globl __kallsyms_count"
		print "__kallsyms_count:"
		printf "\t.4byte %d\n", n
		print ".globl __kallsyms_table"
		print "__kallsyms_table:"
		off = 0
		for (i = 0; i < n; i++) {
			printf "\t.8byte 0x%s\n", addrs[i]
			printf "\t.4byte %d\n",   off
			printf "\t.4byte 0\n"
			off += length(names[i]) + 1
		}
		print ".globl __kallsyms_strings"
		print "__kallsyms_strings:"
		for (i = 0; i < n; i++) {
			printf "\t.asciz \"%s\"\n", names[i]
		}
	}
'
