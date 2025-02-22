#include "HD.hh"
#include "FileContext.hh"
#include "FilePool.hh"
#include "DeviceConfig.hh"
#include "CliComm.hh"
#include "HDImageCLI.hh"
#include "MSXMotherBoard.hh"
#include "Reactor.hh"
#include "Display.hh"
#include "GlobalSettings.hh"
#include "MSXException.hh"
#include "Timer.hh"
#include "serialize.hh"
#include "tiger.hh"
#include <cassert>
#include <memory>

namespace openmsx {

using std::string;

HD::HD(const DeviceConfig& config)
	: motherBoard(config.getMotherBoard())
	, name("hdX")
{
	hdInUse = motherBoard.getSharedStuff<HDInUse>("hdInUse");

	unsigned id = 0;
	while ((*hdInUse)[id]) {
		++id;
		if (id == MAX_HD) {
			throw MSXException("Too many HDs");
		}
	}
	// for exception safety, set hdInUse only at the end
	name[2] = char('a' + id);

	// For the initial hd image, savestate should only try exactly this
	// (resolved) filename. For user-specified hd images (commandline or
	// via hda command) savestate will try to re-resolve the filename.
	auto mode = File::NORMAL;
	string cliImage = HDImageCLI::getImageForId(id);
	if (cliImage.empty()) {
		const auto& original = config.getChildData("filename");
		filename = Filename(config.getFileContext().resolveCreate(original));
		mode = File::CREATE;
	} else {
		filename = Filename(std::move(cliImage), userFileContext());
	}

	file = File(filename, mode);
	filesize = file.getSize();
	if (mode == File::CREATE && filesize == 0) {
		// OK, the file was just newly created. Now make sure the file
		// is of the right (default) size
		file.truncate(size_t(config.getChildDataAsInt("size", 0)) * 1024 * 1024);
		filesize = file.getSize();
	}
	tigerTree.emplace(*this, filesize, filename.getResolved());

	(*hdInUse)[id] = true;
	hdCommand.emplace(
		motherBoard.getCommandController(),
		motherBoard.getStateChangeDistributor(),
		motherBoard.getScheduler(),
		*this,
		motherBoard.getReactor().getGlobalSettings().getPowerSetting());

	motherBoard.getMSXCliComm().update(CliComm::HARDWARE, name, "add");
}

HD::~HD()
{
	motherBoard.getMSXCliComm().update(CliComm::HARDWARE, name, "remove");

	unsigned id = name[2] - 'a';
	assert((*hdInUse)[id]);
	(*hdInUse)[id] = false;
}

void HD::switchImage(const Filename& newFilename)
{
	file = File(newFilename);
	filename = newFilename;
	filesize = file.getSize();
	tigerTree.emplace(*this, filesize, filename.getResolved());
	motherBoard.getMSXCliComm().update(CliComm::MEDIA, getName(),
	                                   filename.getResolved());
}

size_t HD::getNbSectorsImpl() const
{
	return filesize / sizeof(SectorBuffer);
}

void HD::readSectorsImpl(
	SectorBuffer* buffers, size_t startSector, size_t num)
{
	file.seek(startSector * sizeof(SectorBuffer));
	file.read(buffers, num * sizeof(SectorBuffer));
}

void HD::writeSectorImpl(size_t sector, const SectorBuffer& buf)
{
	file.seek(sector * sizeof(buf));
	file.write(&buf, sizeof(buf));
	tigerTree->notifyChange(sector * sizeof(buf), sizeof(buf),
	                        file.getModificationDate());
}

bool HD::isWriteProtectedImpl() const
{
	return file.isReadOnly();
}

Sha1Sum HD::getSha1SumImpl(FilePool& filePool)
{
	if (hasPatches()) {
		return SectorAccessibleDisk::getSha1SumImpl(filePool);
	}
	return filePool.getSha1Sum(file);
}

void HD::showProgress(size_t position, size_t maxPosition)
{
	// only show progress iff:
	//  - 1 second has passed since last progress OR
	//  - we reach completion and did progress before (to show the 100%)
	// This avoids showing any progress if the operation would take less than 1 second.
	auto now = Timer::getTime();
	if (((now - lastProgressTime) > 1000000) ||
	    ((position == maxPosition) && everDidProgress)) {
		lastProgressTime = now;
		int percentage = int((100 * position) / maxPosition);
		motherBoard.getMSXCliComm().printProgress(
			"Calculating hash for ", filename.getResolved(),
			"... ", percentage, '%');
		motherBoard.getReactor().getDisplay().repaint();
		everDidProgress = true;
	}
}

std::string HD::getTigerTreeHash()
{
	lastProgressTime = Timer::getTime();
	everDidProgress = false;
	auto callback = [this](size_t p, size_t t) { showProgress(p, t); };
	return tigerTree->calcHash(callback).toString(); // calls HD::getData()
}

uint8_t* HD::getData(size_t offset, size_t size)
{
	assert(size <= TigerTree::BLOCK_SIZE);
	assert((offset % sizeof(SectorBuffer)) == 0);
	assert((size   % sizeof(SectorBuffer)) == 0);

	struct Work {
		char extra; // at least one byte before 'bufs'
		// likely here are padding bytes in between
		SectorBuffer bufs[TigerTree::BLOCK_SIZE / sizeof(SectorBuffer)];
	};
	static Work work; // not reentrant

	size_t sector = offset / sizeof(SectorBuffer);
	size_t num    = size   / sizeof(SectorBuffer);
	readSectors(work.bufs, sector, num); // This possibly applies IPS patches.
	return work.bufs[0].raw;
}

bool HD::isCacheStillValid(time_t& cacheTime)
{
	time_t fileTime = file.getModificationDate();
	bool result = fileTime == cacheTime;
	cacheTime = fileTime;
	return result;
}

SectorAccessibleDisk* HD::getSectorAccessibleDisk()
{
	return this;
}

std::string_view HD::getContainerName() const
{
	return getName();
}

bool HD::diskChanged()
{
	return false; // TODO not implemented
}

int HD::insertDisk(const std::string& newFilename)
{
	try {
		switchImage(Filename(newFilename));
		return 0;
	} catch (MSXException&) {
		return -1;
	}
}

// version 1: initial version
// version 2: replaced 'checksum'(=sha1) with 'tthsum`
template<typename Archive>
void HD::serialize(Archive& ar, unsigned version)
{
	Filename tmp = file.is_open() ? filename : Filename();
	ar.serialize("filename", tmp);
	if constexpr (Archive::IS_LOADER) {
		if (tmp.empty()) {
			// Lazily open file specified in config. And close if
			// it was already opened (in the constructor). The
			// latter can occur in the following scenario:
			//  - The hd image doesn't exist yet
			//  - Reverse creates savestates, these still have
			//      tmp="" (because file=nullptr)
			//  - At some later point the hd image gets created
			//     (e.g. on first access to the image)
			//  - Now reverse to some point in EmuTime before the
			//    first disk access
			//  - The loadstate re-constructs this HD object, but
			//    because the hd image does exist now, it gets
			//    opened in the constructor (file!=nullptr).
			//  - So to get in the same state as the initial
			//    savestate we again close the file. Otherwise the
			//    checksum-check code below goes wrong.
			file.close();
		} else {
			tmp.updateAfterLoadState();
			if (filename != tmp) switchImage(tmp);
			assert(file.is_open());
		}
	}

	// store/check checksum
	if (file.is_open()) {
		bool mismatch = false;

		if (ar.versionAtLeast(version, 2)) {
			// use tiger-tree-hash
			string oldTiger;
			if constexpr (!Archive::IS_LOADER) {
				oldTiger = getTigerTreeHash();
			}
			ar.serialize("tthsum", oldTiger);
			if constexpr (Archive::IS_LOADER) {
				string newTiger = getTigerTreeHash();
				mismatch = oldTiger != newTiger;
			}
		} else {
			// use sha1
			auto& filepool = motherBoard.getReactor().getFilePool();
			Sha1Sum oldChecksum;
			if constexpr (!Archive::IS_LOADER) {
				oldChecksum = getSha1Sum(filepool);
			}
			string oldChecksumStr = oldChecksum.empty()
					      ? string{}
					      : oldChecksum.toString();
			ar.serialize("checksum", oldChecksumStr);
			oldChecksum = oldChecksumStr.empty()
				    ? Sha1Sum()
				    : Sha1Sum(oldChecksumStr);

			if constexpr (Archive::IS_LOADER) {
				Sha1Sum newChecksum = getSha1Sum(filepool);
				mismatch = oldChecksum != newChecksum;
			}
		}

		if constexpr (Archive::IS_LOADER) {
			if (mismatch) {
				motherBoard.getMSXCliComm().printWarning(
					"The content of the harddisk ",
					tmp.getResolved(),
					" has changed since the time this savestate was "
					"created. This might result in emulation problems "
					"or even diskcorruption. To prevent the latter, "
					"the harddisk is now write-protected.");
				forceWriteProtect();
			}
		}
	}
}
INSTANTIATE_SERIALIZE_METHODS(HD);

} // namespace openmsx
