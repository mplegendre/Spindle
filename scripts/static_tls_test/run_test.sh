#!/bin/sh

CC=$1
CFLAGS=$2
SRCDIR=$3/scripts/static_tls_test
BINDIR=$4/static_tls_test

mkdir -p $BINDIR

if [ ! -f "$BINDIR/statictls" ]; then
$CC $CFLAGS -o $BINDIR/libstatictls.so -shared -fPIC $SRCDIR/statictls.c
$CC $CFLAGS -o $BINDIR/statictls -L$BINDIR -lstatictls -lpthread -Wl,-rpath,$BINDIR $SRCDIR/app.c
$CC $CFLAGS -o $BINDIR/libminaudit.so -fPIC -shared $SRCDIR/minaudit.c
fi
LD_AUDIT=$BINDIR/libminaudit.so $BINDIR/statictls >& /dev/null
RESULT=$?

exit $RESULT