#!/bin/sh --

cat <<EOF
/* this is an autogenerated file.  edit patchlist.sh instead. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "mutt.h"

void mutt_print_patchlist (void)
{
EOF

cat - | while read patch ; do
	echo "  puts (\"${patch}\");"
done

echo "}"
