# Generates version include file.

from __future__ import print_function
from outpututils import rewriteIfChanged
from version import extractRevisionString, packageVersion, releaseFlag

import sys

def iterVersionInclude():
	revision = extractRevisionString()

	yield '// Automatically generated by build process.'
	yield 'const bool Version::RELEASE = %s;' % str(releaseFlag).lower()
	yield 'const char* const Version::VERSION = "%s";' % packageVersion
	yield 'const char* const Version::REVISION = "%s";' % revision

if __name__ == '__main__':
	if len(sys.argv) == 2:
		rewriteIfChanged(sys.argv[1], iterVersionInclude())
	else:
		print('Usage: python version2code.py VERSION_HEADER', file=sys.stderr)
		sys.exit(2)
