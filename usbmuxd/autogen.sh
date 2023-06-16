#!/bin/sh

olddir=`pwd`
srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

(
  cd "$srcdir"

  gprefix=`which glibtoolize 2>&1 >/dev/null`
  if [ $? -eq 0 ]; then
    glibtoolize --force
  else
    libtoolize --force
  fi
  aclocal -I m4
  autoheader
  automake --add-missing
  autoconf

  requires_pkgconfig=`which pkg-config 2>&1 >/dev/null`
  if [ $? -ne 0 ]; then
    echo "Missing required pkg-config. Please install it on your system and run again."
  fi

  cd "$olddir"
)

if [ -z "$NOCONFIGURE" ]; then
  $srcdir/configure "$@"
fi
