#!/bin/sh

# Used to call "./autogen.sh" here
echo "autoreconf -i"
autoreconf -i

echo "chmod +x debian/rules"
chmod +x debian/rules

# in some environments the '-rfakeroot' can cause a failure (e.g. when
# building as root). If so, remove that argument from the following:
echo "dpkg-buildpackage -b -rfakeroot -us -uc"
dpkg-buildpackage -b -rfakeroot -us -uc

# If the above succeeds then the ".deb" binary package is placed in the
# parent directory.
