# Replacement for autoconf.
# Performs some test compiles, to check for headers and functions.
# It does not execute anything it builds, making it friendly for cross compiles.

from __future__ import print_function
from compilers import CompileCommand, LinkCommand
from components import iterComponents, requiredLibrariesFor
from configurations import getConfiguration
from executils import captureStdout, shjoin
from itertools import chain
from libraries import librariesByName
from makeutils import extractMakeVariables, parseBool
from outpututils import rewriteIfChanged
from packages import getPackage
from systemfuncs import systemFunctions
from systemfuncs2code import iterSystemFuncsHeader

from io import open
from os import environ, makedirs, remove
from os.path import isdir, isfile, pathsep
from shlex import split as shsplit
import sys

def resolve(log, expr):
	if expr is None:
		return ''
	# TODO: Since for example "sdl-config" is used in more than one
	#       CFLAGS definition, it will be executed multiple times.
	try:
		return normalizeWhitespace(evaluateBackticks(log, expr))
	except IOError:
		# Executing a lib-config script is expected to fail if the
		# script is not installed.
		# TODO: Report this explicitly in the probe results table.
		return ''

def writeFile(path, lines):
	with open(path, 'w', encoding='utf-8') as out:
		for line in lines:
			print(line, file=out)

def tryCompile(log, compileCommand, sourcePath, lines):
	'''Write the program defined by "lines" to a text file specified
	by "path" and try to compile it.
	Returns True iff compilation succeeded.
	'''
	assert sourcePath.endswith('.cc')
	objectPath = sourcePath[ : -3] + '.o'
	writeFile(sourcePath, lines)
	try:
		return compileCommand.compile(log, sourcePath, objectPath)
	finally:
		remove(sourcePath)
		if isfile(objectPath):
			remove(objectPath)

def checkCompiler(log, compileCommand, outDir):
	'''Checks whether compiler can compile anything at all.
	Returns True iff the compiler works.
	'''
	def hello():
		# The most famous program.
		yield '#include <iostream>'
		yield 'int main(int argc, char** argv) {'
		yield '  std::cout << "Hello World!" << std::endl;'
		yield '  return 0;'
		yield '}'
	return tryCompile(log, compileCommand, outDir + '/hello.cc', hello())

def checkFunc(log, compileCommand, outDir, checkName, funcName, headers):
	'''Checks whether the given function is declared by the given headers.
	Returns True iff the function is declared.
	'''
	def takeFuncAddr():
		# Try to include the necessary headers and get the function address.
		for header in headers:
			yield '#include %s' % header
		yield 'void (*f)() = reinterpret_cast<void (*)()>(%s);' % funcName
	return tryCompile(
		log, compileCommand, outDir + '/' + checkName + '.cc', takeFuncAddr()
		)

def evaluateBackticks(log, expression):
	parts = []
	index = 0
	while True:
		start = expression.find('`', index)
		if start == -1:
			parts.append(expression[index : ])
			break
		end = expression.find('`', start + 1)
		if end == -1:
			raise ValueError('Unmatched backtick: %s' % expression)
		parts.append(expression[index : start])
		command = expression[start + 1 : end].strip()
		result = captureStdout(log, command)
		if result is None:
			raise IOError('Backtick evaluation failed; see log')
		parts.append(result)
		index = end + 1
	return ''.join(parts)

def normalizeWhitespace(expression):
	return shjoin(shsplit(expression))

class TargetSystem(object):

	def __init__(
		self, log, logPath, compileCommandStr, outDir, platform, distroRoot,
		configuration
		):
		'''Create empty log and result files.
		'''
		self.log = log
		self.logPath = logPath
		self.compileCommandStr = compileCommandStr
		self.outDir = outDir
		self.platform = platform
		self.distroRoot = distroRoot
		self.configuration = configuration
		self.outMakePath = outDir + '/probed_defs.mk'
		self.outHeaderPath = outDir + '/systemfuncs.hh'
		self.outVars = {}
		self.functionResults = {}
		self.typeTraitsResult = None
		self.libraries = sorted(requiredLibrariesFor(
			configuration.iterDesiredComponents()
			))

	def checkAll(self):
		'''Run all probes.
		'''
		self.hello()
		for func in systemFunctions:
			self.checkFunc(func)
		for library in self.libraries:
			self.checkLibrary(library)

	def writeAll(self):
		def iterVars():
			yield '# Automatically generated by build system.'
			yield '# Non-empty value means found, empty means not found.'
			for library in self.libraries:
				for name in (
					'HAVE_%s_H' % library,
					'HAVE_%s_LIB' % library,
					'%s_CFLAGS' % library,
					'%s_LDFLAGS' % library,
					):
					yield '%s:=%s' % (name, self.outVars[name])
		rewriteIfChanged(self.outMakePath, iterVars())

		rewriteIfChanged(
			self.outHeaderPath,
			iterSystemFuncsHeader(self.functionResults),
			)

	def printResults(self):
		for line in iterProbeResults(
			self.outVars, self.configuration, self.logPath
			):
			print(line)

	def everything(self):
		self.checkAll()
		self.writeAll()
		self.printResults()

	def hello(self):
		'''Check compiler with the most famous program.
		'''
		compileCommand = CompileCommand.fromLine(self.compileCommandStr, '')
		ok = checkCompiler(self.log, compileCommand, self.outDir)
		print('Compiler %s: %s' % (
			'works' if ok else 'broken',
			compileCommand
			), file=self.log)
		self.outVars['COMPILER'] = str(ok).lower()

	def checkFunc(self, func):
		'''Probe for function.
		'''
		compileCommand = CompileCommand.fromLine(self.compileCommandStr, '')
		ok = checkFunc(
			self.log, compileCommand, self.outDir,
			func.name, func.getFunctionName(), func.iterHeaders(self.platform)
			)
		print('%s function: %s' % (
			'Found' if ok else 'Missing',
			func.getFunctionName()
			), file=self.log)
		self.functionResults[func.getMakeName()] = ok

	def checkLibrary(self, makeName):
		library = librariesByName[makeName]
		cflags = resolve(
			self.log,
			library.getCompileFlags(
				self.platform, self.configuration.linkStatic(), self.distroRoot
				)
			)
		ldflags = resolve(
			self.log,
			library.getLinkFlags(
				self.platform, self.configuration.linkStatic(), self.distroRoot
				)
			)
		self.outVars['%s_CFLAGS' % makeName] = cflags
		self.outVars['%s_LDFLAGS' % makeName] = ldflags

		sourcePath = self.outDir + '/' + makeName + '.cc'
		objectPath = self.outDir + '/' + makeName + '.o'
		binaryPath = self.outDir + '/' + makeName + '.bin'
		if self.platform == 'android':
			binaryPath = self.outDir + '/' + makeName + '.so'
			ldflags += ' -shared -Wl,--no-undefined'

		compileCommand = CompileCommand.fromLine(self.compileCommandStr, cflags)
		linkCommand = LinkCommand.fromLine(self.compileCommandStr, ldflags)

		funcName = library.function
		headers = library.getHeaders(self.platform)
		def takeFuncAddr():
			# Try to include the necessary headers and get the function address.
			for header in headers:
				yield '#include %s' % header
			yield 'void (*f)() = reinterpret_cast<void (*)()>(%s);' % funcName
			yield 'int main(int argc, char** argv) {'
			yield '  return 0;'
			yield '}'
		writeFile(sourcePath, takeFuncAddr())
		try:
			compileOK = compileCommand.compile(self.log, sourcePath, objectPath)
			print('%s: %s header' % (
				makeName,
				'Found' if compileOK else 'Missing'
				), file=self.log)
			if compileOK:
				linkOK = linkCommand.link(self.log, [ objectPath ], binaryPath)
				print('%s: %s lib' % (
					makeName,
					'Found' if linkOK else 'Missing'
					), file=self.log)
			else:
				linkOK = False
				print((
					'%s: Cannot test linking because compile failed'
					% makeName
					), file=self.log)
		finally:
			remove(sourcePath)
			if isfile(objectPath):
				remove(objectPath)
			if isfile(binaryPath):
				remove(binaryPath)

		self.outVars['HAVE_%s_H' % makeName] = 'true' if compileOK else ''
		self.outVars['HAVE_%s_LIB' % makeName] = 'true' if linkOK else ''
		if linkOK:
			versionGet = library.getVersion(
				self.platform, self.configuration.linkStatic(), self.distroRoot
				)
			if callable(versionGet):
				version = versionGet(compileCommand, self.log)
			else:
				version = resolve(self.log, versionGet)
			if version is None:
				version = 'error'
			self.outVars['VERSION_%s' % makeName] = version

def iterProbeResults(probeVars, configuration, logPath):
	'''Present probe results, so user can decide whether to start the build,
	or to change system configuration and rerun "configure".
	'''
	desiredComponents = set(configuration.iterDesiredComponents())
	requiredComponents = set(configuration.iterRequiredComponents())
	buildableComponents = set(configuration.iterBuildableComponents(probeVars))
	packages = sorted(
		(	getPackage(makeName)
			for makeName in requiredLibrariesFor(desiredComponents)
			),
		key = lambda package: package.niceName.lower()
		)
	customVars = extractMakeVariables('build/custom.mk')

	yield ''
	if not parseBool(probeVars['COMPILER']):
		yield 'No working C++ compiler was found.'
		yield "Please install a C++ compiler, such as GCC's g++."
		yield 'If you have a C++ compiler installed and openMSX did not ' \
			'detect it, please set the environment variable CXX to the name ' \
			'of your C++ compiler.'
		yield 'After you have corrected the situation, rerun "configure".'
		yield ''
	else:
		# Compute how wide the first column should be.
		def iterNiceNames():
			for package in packages:
				yield package.niceName
			for component in iterComponents():
				yield component.niceName
		maxLen = max(len(niceName) for niceName in iterNiceNames())
		formatStr = '  %-' + str(maxLen + 3) + 's %s'

		yield 'Found libraries:'
		for package in packages:
			makeName = package.getMakeName()
			if probeVars['HAVE_%s_LIB' % makeName]:
				found = 'version %s' % probeVars['VERSION_%s' % makeName]
			elif probeVars['HAVE_%s_H' % makeName]:
				# Dependency resolution of a typical distro will not allow
				# this situation. Most likely we got the link flags wrong.
				found = 'headers found, link test failed'
			else:
				found = 'no'
			yield formatStr % (package.niceName + ':', found)
		yield ''

		yield 'Components overview:'
		for component in iterComponents():
			if component in desiredComponents:
				status = 'yes' if component in buildableComponents else 'no'
			else:
				status = 'disabled'
			yield formatStr % (component.niceName + ':', status)
		yield ''

		yield 'Customisable options:'
		yield formatStr % ('Install to', customVars['INSTALL_BASE'])
		yield '  (you can edit these in build/custom.mk)'
		yield ''

		if buildableComponents == desiredComponents:
			yield 'All required and optional components can be built.'
		elif requiredComponents.issubset(buildableComponents):
			yield 'If you are satisfied with the probe results, ' \
				'run "make" to start the build.'
			yield 'Otherwise, install some libraries and headers ' \
				'and rerun "configure".'
		else:
			yield 'Please install missing libraries and headers ' \
				'and rerun "configure".'
		yield ''
		yield 'If the detected libraries differ from what you think ' \
			'is installed on this system, please check the log file: %s' \
			% logPath
		yield ''

def main(compileCommandStr, outDir, platform, linkMode, thirdPartyInstall):
	if not isdir(outDir):
		makedirs(outDir)
	logPath = outDir + '/probe.log'
	with open(logPath, 'w', encoding='utf-8') as log:
		print('Probing target system...')
		print('Probing system:', file=log)
		distroRoot = thirdPartyInstall or None
		if distroRoot is None:
			if platform == 'darwin':
				for searchPath in environ.get('PATH', '').split(pathsep):
					if searchPath == '/opt/local/bin':
						print('Using libraries from MacPorts.')
						distroRoot = '/opt/local'
						break
					elif searchPath == '/sw/bin':
						print('Using libraries from Fink.')
						distroRoot = '/sw'
						break
				else:
					distroRoot = '/usr/local'
			elif platform.endswith('bsd') or platform == 'dragonfly':
				distroRoot = environ.get('LOCALBASE', '/usr/local')
				print('Using libraries from ports directory %s.' % distroRoot)
			elif platform == 'pandora':
				distroRoot = environ.get('LIBTOOL_SYSROOT_PATH')
				if distroRoot is not None:
					distroRoot += '/usr'
					print(
						'Using libraries from sysroot directory %s.'
						% distroRoot
						)

		configuration = getConfiguration(linkMode)

		TargetSystem(
			log, logPath, compileCommandStr, outDir, platform, distroRoot,
			configuration
			).everything()

if __name__ == '__main__':
	if len(sys.argv) == 6:
		try:
			main(*sys.argv[1 : ])
		except ValueError as ve:
			print(ve, file=sys.stderr)
			sys.exit(2)
	else:
		print(
			'Usage: python probe.py '
			'COMPILE OUTDIR OPENMSX_TARGET_OS LINK_MODE 3RDPARTY_INSTALL_DIR',
			file=sys.stderr
			)
		sys.exit(2)
