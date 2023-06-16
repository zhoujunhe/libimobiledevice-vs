#!/bin/bash

# Patch the dylib dependencies for all executables in the out directory
# Redirect from libimobiledevice.6.dylib in the out folder to libimobiledevice.dylib
# in the folder where the executable is located (this will be the setup in our target
# environment)
for f in $INSTALLDIR/sbin/*; do
   for l in libplist libusbmuxd libimobiledevice; do
     chmod +w $f

     # Skip the first line of the otool output, this is just the header
     dylibs=`otool -L $f | tail -n +2 | grep "$l" | awk -F' ' '{ print $1 }'`

     for dylib in $dylibs; do
       echo Patching $dylib in $f

       # https://www.mikeash.com/pyblog/friday-qa-2009-11-06-linking-and-install-names.html
       install_name_tool -change $dylib @loader_path/$l.dylib $f
     done
   done

   otool -L $f
done

install_name_tool -change /usr/local/opt/libusb/lib/libusb-1.0.0.dylib @loader_path/libusb-1.0.0.dylib $INSTALLDIR/sbin/usbmuxd
