#include "patchmaker.hpp"

#include <fstream>
#include <functional>
#include <iomanip>

#include "../elf.hpp"

#include "../main.hpp"
#include "../log.hpp"
#include "../except.hpp"
#include "../config/buildconfig.hpp"
#include "../config/rebuildconfig.hpp"
#include "../util.hpp"
#include "../process.hpp"

/*
 * TODO: Endianness checks
 * */

#define OSTRa(x) ANSI_bWHITE "\"" << (x) << "\"" ANSI_RESET

namespace fs = std::filesystem;

constexpr std::size_t SizeOfHookBridge = 20;

struct PatchType {
	enum {
		Jump, Call, Hook, Over,
		SetJump, SetCall, SetHook,
		RtRepl
	};
};

struct GenericPatchInfo
{
	u32 srcAddress; // the address of the symbol (only fetched after linkage)
	int srcAddressOv; // the overlay the address of the symbol (-1 arm, >= 0 overlay)
	u32 destAddress; // the address to be patched
	int destAddressOv; // the overlay of the address to be patched
	std::size_t patchType; // the patch type
	int sectionIdx; // the index of the section (-1 label, >= 0 section index)
	int sectionSize; // the size of the section (used for over patches)
	std::string symbol; // the symbol of the patch (used to generate linker script)
	SourceFileJob* job;
};

struct RtReplPatchInfo
{
	std::string symbol;
	SourceFileJob* job;
};

struct NewcodePatch
{
	const u8* binData;
	const u8* bssData;
	std::size_t binSize;
	std::size_t binAlign;
	std::size_t bssSize;
	std::size_t bssAlign;
};

struct HookMakerInfo
{
	u32 address;
	u32 curAddress;
	std::vector<u8> data;
};

struct LDSMemoryEntry
{
	std::string name;
	u32 origin;
	int length;
};

struct LDSRegionEntry
{
	int dest;
	LDSMemoryEntry* memory;
	const BuildTarget::Region* region;
	std::vector<GenericPatchInfo*> sectionPatches;
};

struct LDSOverPatch
{
	GenericPatchInfo* info;
	LDSMemoryEntry* memory;
};

static const char* s_patchTypeNames[] = {
	"jump", "call", "hook", "over",
	"setjump", "setcall", "sethook",
	"rtrepl"
};

static void forEachElfSection(
	const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl, const char* str_tbl,
	const std::function<bool(std::size_t, const Elf32_Shdr&, std::string_view)>& cb
)
{
	for (std::size_t i = 0; i < eh.e_shnum; i++)
	{
		const Elf32_Shdr& sh = sh_tbl[i];
		std::string_view sectionName(&str_tbl[sh.sh_name]);
		if (cb(i, sh, sectionName))
			break;
	}
}

static void forEachElfSymbol(
	const Elf32& elf, const Elf32_Ehdr& eh, const Elf32_Shdr* sh_tbl,
	const std::function<bool(const Elf32_Sym&, std::string_view)>& cb
)
{
	for (std::size_t i = 0; i < eh.e_shnum; i++)
	{
		const Elf32_Shdr& sh = sh_tbl[i];
		if ((sh.sh_type == SHT_SYMTAB) || (sh.sh_type == SHT_DYNSYM))
		{
			auto sym_tbl = elf.getSection<Elf32_Sym>(sh);
			auto sym_str_tbl = elf.getSection<char>(sh_tbl[sh.sh_link]);
			for (std::size_t j = 0; j < sh.sh_size / sizeof(Elf32_Sym); j++)
			{
				const Elf32_Sym& sym = sym_tbl[j];
				std::string_view symbolName(&sym_str_tbl[sym.st_name]);
				if (cb(sym, symbolName))
					break;
			}
		}
	}
}

PatchMaker::PatchMaker() = default;
PatchMaker::~PatchMaker() = default;

void PatchMaker::makeTarget(
	const BuildTarget& target,
	const std::filesystem::path& targetWorkDir,
	const std::filesystem::path& buildDir,
	const HeaderBin& header,
	std::vector<std::unique_ptr<SourceFileJob>>& srcFileJobs
	)
{
	m_target = &target;
	m_targetWorkDir = &targetWorkDir;
	m_buildDir = &buildDir;
	m_header = &header;
	m_srcFileJobs = &srcFileJobs;

	m_ldscriptPath = *m_buildDir / (m_target->getArm9() ? "ldscript9.x" : "ldscript7.x");
	m_elfPath = *m_buildDir / (m_target->getArm9() ? "arm9.elf" : "arm7.elf");

	createBackupDirectory();

	loadArmBin();
	loadOverlayTableBin();

	std::vector<u32>& patchedOverlays = m_target->getArm9() ?
		RebuildConfig::getArm7PatchedOvs() :
		RebuildConfig::getArm9PatchedOvs();
	
	for (u32 ovID : patchedOverlays)
		loadOverlayBin(ovID);

	fetchNewcodeAddr();
	gatherInfoFromObjects();
	createLinkerScript();
	linkElfFile();
	loadElfFile();
	gatherInfoFromElf();
	applyPatchesToRom();
	unloadElfFile();

	patchedOverlays.clear();
	for (const auto& [id, ov] : m_loadedOverlays)
	{
		if (ov->getDirty())
			patchedOverlays.push_back(id);
	}

	saveOverlayBins();
	saveOverlayTableBin();
	saveArmBin();
}

void PatchMaker::fetchNewcodeAddr()
{
	m_newcodeAddrForDest[-1] = getArm()->read<u32>(m_target->arenaLo);
	for (auto& region : m_target->regions)
	{
		int dest = region.destination;
		if (dest != -1)
		{
			u32 addr;
			switch (region.mode)
			{
			case BuildTarget::Mode::Append:
			{
				auto& ovtEntry = m_ovtEntries[dest];
				addr = ovtEntry->ramAddress + ovtEntry->ramSize + ovtEntry->bssSize;
				break;
			}
			case BuildTarget::Mode::Replace:
			{
				addr = (region.address == 0xFFFFFFFF) ? m_ovtEntries[dest]->ramAddress : region.address;
				break;
			}
			case BuildTarget::Mode::Create:
			{
				addr = region.address;
				break;
			}
			}
			m_newcodeAddrForDest[dest] = addr;
		}
	}
}

void PatchMaker::gatherInfoFromObjects()
{
	fs::current_path(*m_targetWorkDir);

	Log::info("Getting patches from objects...");

	for (auto& srcFileJob : *m_srcFileJobs)
	{
		const fs::path& objPath = srcFileJob->objFilePath;

		if (Main::getVerbose())
			Log::out << ANSI_bYELLOW << objPath.string() << ANSI_RESET << std::endl;

		const BuildTarget::Region* region = srcFileJob->region;

		std::vector<GenericPatchInfo*> patchInfoForThisObj;

		if (!std::filesystem::exists(objPath))
			throw ncp::file_error(objPath, ncp::file_error::find);
		Elf32 elf;
		if (!elf.load(objPath))
			throw ncp::file_error(objPath, ncp::file_error::read);

		const Elf32_Ehdr& eh = elf.getHeader();
		auto sh_tbl = elf.getSectionHeaderTable();
		auto str_tbl = elf.getSection<char>(sh_tbl[eh.e_shstrndx]);

		auto parseSymbol = [&](std::string_view symbolName, int sectionIdx, int sectionSize){
			std::string_view labelName = symbolName.substr(sectionIdx != -1 ? 5 : 4);

			std::size_t patchTypeNameEnd = labelName.find('_');
			if (patchTypeNameEnd == std::string::npos)
				return;

			std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
			std::size_t patchType = Util::indexOf(patchTypeName, s_patchTypeNames, sizeof(s_patchTypeNames) / sizeof(char*));
			if (patchType == -1)
			{
				Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
				return;
			}

			if (patchType == PatchType::Over && sectionIdx == -1)
			{
				Log::out << OWARN << "\"over\" patch must be a section type patch: " << patchTypeName << std::endl;
				return;
			}

			if (patchType == PatchType::RtRepl)
			{
				if (sectionIdx != -1) // we do not want the labels, those are placeholders
				{
					m_rtreplPatches.emplace_back(new RtReplPatchInfo{
						/*.symbol = */std::string(symbolName),
						/*.job = */srcFileJob.get()
					});
				}
				return;
			}

			bool expectingOverlay = true;
			std::size_t addressNameStart = patchTypeNameEnd + 1;
			std::size_t addressNameEnd = labelName.find('_', addressNameStart);
			if (addressNameEnd == std::string::npos)
			{
				addressNameEnd = labelName.length();
				expectingOverlay = false;
			}
			std::string_view addressName = labelName.substr(addressNameStart, addressNameEnd - addressNameStart);
			u32 destAddress;
			try {
				destAddress = Util::addrToInt(std::string(addressName));
			} catch (std::exception& e) {
				Log::out << OWARN << "Found invalid address for patch: " << labelName << std::endl;
				return;
			}

			int destAddressOv = -1;
			if (expectingOverlay)
			{
				std::size_t overlayNameStart = addressNameEnd + 1;
				std::size_t overlayNameEnd = labelName.length();
				std::string_view overlayName = labelName.substr(overlayNameStart, overlayNameEnd - overlayNameStart);
				if (!overlayName.starts_with("ov"))
				{
					Log::out << OWARN << "Expected overlay definition in patch for: " << labelName << std::endl;
					return;
				}
				try {
					destAddressOv = Util::addrToInt(std::string(overlayName.substr(2)));
				} catch (std::exception& e) {
					Log::out << OWARN << "Found invalid overlay for patch: " << labelName << std::endl;
					return;
				}
			}

			int srcAddressOv = patchType == PatchType::Over ? destAddressOv : region->destination;

			auto* patchInfoEntry = new GenericPatchInfo({
				.srcAddress = 0, // we do not yet know it, only after linkage
				.srcAddressOv = srcAddressOv,
				.destAddress = destAddress,
				.destAddressOv = destAddressOv,
				.patchType = patchType,
				.sectionIdx = sectionIdx,
				.sectionSize = sectionSize,
				.symbol = std::string(symbolName),
				.job = srcFileJob.get()
			});

			patchInfoForThisObj.emplace_back(patchInfoEntry);
			m_patchInfo.emplace_back(patchInfoEntry);
		};

		// Find patches in sections
		forEachElfSection(eh, sh_tbl, str_tbl,
		[&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
			if (sectionName.starts_with(".ncp_"))
			{
				if (sectionName.substr(5).starts_with("set"))
				{
					int dest = region->destination;
					if (std::find(m_destWithNcpSet.begin(), m_destWithNcpSet.end(), dest) == m_destWithNcpSet.end())
						m_destWithNcpSet.emplace_back(dest);
					m_jobsWithNcpSet.emplace_back(srcFileJob.get());
					return false;
				}
				parseSymbol(sectionName, int(sectionIdx), int(section.sh_size));
			}
			return false;
		});

		// Find patches in symbols
		forEachElfSymbol(elf, eh, sh_tbl,
		[&](const Elf32_Sym& symbol, std::string_view symbolName){
			if (symbolName.starts_with("ncp_"))
			{
				std::string_view stemless = symbolName.substr(4);
				if (stemless != "dest" && !stemless.starts_with("set"))
					parseSymbol(symbolName, -1, 0);
			}
			return false;
		});

		// Find functions that should be external (label marked)
		for (GenericPatchInfo* p : patchInfoForThisObj)
		{
			if (p->sectionIdx == -1) // is patch instructed by label
				m_externSymbols.emplace_back(p->symbol);
		}

		if (Main::getVerbose())
		{
			if (patchInfoForThisObj.empty())
			{
				Log::out << "NO PATCHES" << std::endl;
			}
			else
			{
				Log::out << "SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SECTION_IDX, SECTION_SIZE, SYMBOL" << std::endl;
				for (auto& p : patchInfoForThisObj)
				{
					Log::out <<
						std::setw(11) << std::dec << p->srcAddressOv << "  " <<
						std::setw(8) << std::hex << p->destAddress << "  " <<
						std::setw(11) << std::dec << p->destAddressOv << "  " <<
						std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
						std::setw(11) << std::dec << p->sectionIdx << "  " <<
						std::setw(12) << std::dec << p->sectionSize << "  " <<
						std::setw(6) << p->symbol << std::endl;
				}
			}
		}
	}
	if (Main::getVerbose())
	{
		if (m_externSymbols.empty())
		{
			Log::out << "\nExternal symbols: NONE\n" << std::endl;
		}
		else
		{
			Log::out << "\nExternal symbols:\n";
			for (const std::string& sym : m_externSymbols)
				Log::out << sym << '\n';
			Log::out << std::flush;
		}
	}
}

void PatchMaker::createBackupDirectory()
{
	const fs::path& bakDir = BuildConfig::getBackupDir();
	fs::current_path(Main::getWorkPath());
	if (!fs::exists(bakDir))
	{
		if (!fs::create_directories(bakDir))
		{
			std::ostringstream oss;
			oss << "Could not create backup directory: " << OSTR(bakDir);
			throw ncp::exception(oss.str());
		}
	}

	const char* prefix = m_target->getArm9() ? "overlay9" : "overlay7";
	fs::path bakOvDir = bakDir / prefix;
	if (!fs::exists(bakOvDir))
	{
		if (!fs::create_directories(bakOvDir))
		{
			std::ostringstream oss;
			oss << "Could not create overlay backup directory: " << OSTR(bakOvDir);
			throw ncp::exception(oss.str());
		}
	}
}

void PatchMaker::loadArmBin()
{
	bool isArm9 = m_target->getArm9();

	const char* binName; u32 entryAddress, ramAddress, autoLoadListHookOff;
	if (isArm9)
	{
		binName = "arm9.bin";
		entryAddress = m_header->arm9.entryAddress;
		ramAddress = m_header->arm9.ramAddress;
		autoLoadListHookOff = m_header->arm9AutoLoadListHookOffset;
	}
	else
	{
		binName = "arm7.bin";
		entryAddress = m_header->arm7.entryAddress;
		ramAddress = m_header->arm7.ramAddress;
		autoLoadListHookOff = m_header->arm7AutoLoadListHookOffset;
	}

	fs::current_path(Main::getWorkPath());

	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	m_arm = std::make_unique<ArmBin>();
	if (fs::exists(bakBinName)) //has backup
	{
		m_arm->load(bakBinName, entryAddress, ramAddress, autoLoadListHookOff, isArm9);
	}
	else //has no backup
	{
		fs::current_path(Main::getRomPath());
		m_arm->load(binName, entryAddress, ramAddress, autoLoadListHookOff, isArm9);
		const std::vector<u8>& bytes = m_arm->data();

		fs::current_path(Main::getWorkPath());
		std::ofstream outputFile(bakBinName, std::ios::binary);
		if (!outputFile.is_open())
			throw ncp::file_error(bakBinName, ncp::file_error::write);
		outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
		outputFile.close();
	}
}

void PatchMaker::saveArmBin()
{
	const char* binName = m_target->getArm9() ? "arm9.bin" : "arm7.bin";

	const std::vector<u8>& bytes = m_arm->data();

	fs::current_path(Main::getRomPath());
	std::ofstream outputFile(binName, std::ios::binary);
	if (!outputFile.is_open())
		throw ncp::file_error(binName, ncp::file_error::write);
	outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
	outputFile.close();
}

void PatchMaker::loadOverlayTableBin()
{
	Log::info("Loading overlay table...");

	const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

	fs::current_path(Main::getWorkPath());

	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	fs::path workBinName;
	if (fs::exists(bakBinName)) //has backup
	{
		workBinName = bakBinName;
	}
	else //has no backup
	{
		fs::current_path(Main::getRomPath());
		workBinName = binName;
		if (!fs::exists(binName))
			throw ncp::file_error(binName, ncp::file_error::find);
		fs::copy_file(binName, Main::getWorkPath() / bakBinName);
	}

	uintmax_t fileSize = fs::file_size(workBinName);
	u32 overlayCount = fileSize / sizeof(OvtEntry);

	m_ovtEntries.resize(overlayCount);

	std::ifstream inputFile(workBinName, std::ios::binary);
	if (!inputFile.is_open())
		throw ncp::file_error(workBinName, ncp::file_error::read);
	for (u32 i = 0; i < overlayCount; i++)
	{
		auto* entry = new OvtEntry();
		inputFile.read(reinterpret_cast<char*>(entry), sizeof(OvtEntry));
		m_ovtEntries[i] = std::unique_ptr<OvtEntry>(entry);
	}
	inputFile.close();
}

void PatchMaker::saveOverlayTableBin()
{
	fs::current_path(Main::getRomPath());

	const char* binName = m_target->getArm9() ? "arm9ovt.bin" : "arm7ovt.bin";

	std::ofstream outputFile(binName, std::ios::binary);
	if (!outputFile.is_open())
		throw ncp::file_error(binName, ncp::file_error::read);
	for (auto& ovtEntry : m_ovtEntries)
		outputFile.write(reinterpret_cast<const char*>(ovtEntry.get()), sizeof(OvtEntry));
	outputFile.close();
}

OverlayBin* PatchMaker::loadOverlayBin(std::size_t ovID)
{
	std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

	fs::current_path(Main::getWorkPath());

	fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");
	fs::path bakBinName = BuildConfig::getBackupDir() / binName;

	OvtEntry& ovte = *m_ovtEntries[ovID];

	auto* overlay = new OverlayBin();
	if (fs::exists(bakBinName)) //has backup
	{
		overlay->load(bakBinName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
		ovte.flag = 0;
	}
	else //has no backup
	{
		fs::current_path(Main::getRomPath());
		overlay->load(binName, ovte.ramAddress, ovte.flag & OVERLAY_FLAG_COMP, ovID);
		ovte.flag = 0;
		const std::vector<u8>& bytes = overlay->data();

		fs::current_path(Main::getWorkPath());
		std::ofstream outputFile(bakBinName, std::ios::binary);
		if (!outputFile.is_open())
			throw ncp::file_error(bakBinName, ncp::file_error::write);
		outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
		outputFile.close();
	}

	m_loadedOverlays.emplace(ovID, std::unique_ptr<OverlayBin>(overlay));
	return overlay;
}

OverlayBin* PatchMaker::getOverlay(std::size_t ovID)
{
	for (auto& [id, ov] : m_loadedOverlays)
	{
		if (id == ovID)
			return ov.get();
	}
	return loadOverlayBin(ovID);
}

void PatchMaker::saveOverlayBins()
{
	std::string prefix = m_target->getArm9() ? "overlay9" : "overlay7";

	fs::current_path(Main::getRomPath());

	for (auto& [ovID, ov] : m_loadedOverlays)
	{
		fs::path binName = fs::path(prefix) / (prefix + "_" + std::to_string(ovID) + ".bin");

		const std::vector<u8>& bytes = ov->data();

		std::ofstream outputFile(binName, std::ios::binary);
		if (!outputFile.is_open())
			throw ncp::file_error(binName, ncp::file_error::write);
		outputFile.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
		outputFile.close();
	}
}

void PatchMaker::createLinkerScript()
{
	auto addSectionInclude = [](std::string& o, std::string& objPath, const char* secInc){
		o += "\t\t\"";
		o += objPath;
		o += "\" (.";
		o += secInc;
		o += ")\n";
	};

	Log::out << OLINK << "Generating the linker script..." << std::endl;

	fs::current_path(*m_targetWorkDir);
	fs::path symbolsFile = fs::absolute(m_target->symbols);

	fs::current_path(*m_buildDir);

	std::vector<std::unique_ptr<LDSMemoryEntry>> memoryEntries;
	memoryEntries.emplace_back(new LDSMemoryEntry{ "bin", 0, 0x100000 });

	std::vector<std::unique_ptr<LDSRegionEntry>> regionEntries;

	// Overlays must come before arm section
	std::vector<const BuildTarget::Region*> orderedRegions(m_target->regions.size());
	for (std::size_t i = 0; i < m_target->regions.size(); i++)
		orderedRegions[i] = &m_target->regions[i];
	std::sort(orderedRegions.begin(), orderedRegions.end(), [](const BuildTarget::Region* a, const BuildTarget::Region* b){
		return a->destination > b->destination;
	});

	for (const BuildTarget::Region* region : orderedRegions)
	{
		LDSMemoryEntry* memEntry;

		int dest = region->destination;
		u32 newcodeAddr = m_newcodeAddrForDest[dest];
		if (dest == -1)
		{
			memEntry = new LDSMemoryEntry{ "arm", newcodeAddr, region->length };
		}
		else
		{
			std::string memName; memName.reserve(8);
			memName += "ov";
			memName += std::to_string(dest);
			memEntry = new LDSMemoryEntry{ std::move(memName), newcodeAddr, region->length };
		}

		memoryEntries.emplace_back(memEntry);
		regionEntries.emplace_back(new LDSRegionEntry{ dest, memEntry, region });
	}

	std::vector<std::unique_ptr<LDSOverPatch>> overPatches;

	// Iterate all patches to setup the linker script
	for (auto& info : m_patchInfo)
	{
		if (info->patchType == PatchType::Over)
		{
			std::string memName; memName.reserve(32);
			memName += "over_";
			memName += Util::intToAddr(int(info->destAddress), 8, false);
			if (info->destAddressOv != -1)
			{
				memName += '_';
				memName += std::to_string(info->destAddressOv);
			}
			auto* memEntry = new LDSMemoryEntry({ std::move(memName), info->destAddress, info->sectionSize });
			memoryEntries.emplace_back(memEntry);
			overPatches.emplace_back(new LDSOverPatch{ info.get(), memEntry });
		}
		else
		{
			for (auto& ldsRegion : regionEntries)
			{
				if (ldsRegion->dest == info->job->region->destination && info->sectionIdx != -1)
					ldsRegion->sectionPatches.emplace_back(info.get());
			}
		}
	}

	if (!m_destWithNcpSet.empty())
		memoryEntries.emplace_back(new LDSMemoryEntry{ "ncp_set", 0, 0x100000 });

	std::string o;
	o.reserve(65536);

	o += "/* NCPatcher: Auto-generated linker script */\n\nINCLUDE \"";

	o += fs::relative(symbolsFile).string();
	o += "\"\n\nINPUT (\n";

	for (auto& srcFileJob : *m_srcFileJobs)
	{
		o += "\t\"";
		o += fs::relative(srcFileJob->objFilePath).string();
		o += "\"\n";
	}

	o += ")\n\nOUTPUT (\"";
	o += fs::relative(m_elfPath).string();
	o += "\")\n\nMEMORY {\n";

	for (auto& memoryEntry : memoryEntries)
	{
		o += '\t';
		o += memoryEntry->name;
		o += " (rwx): ORIGIN = ";
		o += Util::intToAddr(int(memoryEntry->origin), 8);
		o += ", LENGTH = ";
		o += Util::intToAddr(int(memoryEntry->length), 8);
		o += '\n';
	}

	o += "}\n\nSECTIONS {\n";

	for (auto& s : regionEntries)
	{
		std::size_t hookCount = 0;
		// TEXT
		o += "\t.";
		o += s->memory->name;
		o += ".text : ALIGN(4) {\n";
		for (auto& p : s->sectionPatches)
		{
			// Convert the section patches into label patches,
			// except for over and set types
			o += "\t\t";
			o += std::string_view(p->symbol).substr(1);
			o += " = .;\n\t\tKEEP(* (";
			o += p->symbol;
			o += "))\n";
			if (p->patchType == PatchType::Hook)
				hookCount++;
		}
		for (auto& p : m_rtreplPatches)
		{
			if (p->job->region == s->region)
			{
				std::string_view stem = std::string_view(p->symbol).substr(1);
				o += "\t\t";
				o += stem;
				o += "_start = .;\n\t\t* (";
				o += p->symbol;
				o += ")\n\t\t";
				o += stem;
				o += "_end = .;\n";
			}
		}
		if (s->dest == -1)
		{
			o += "\t\t* (.text)\n"
				 "\t\t* (.rodata)\n"
				 "\t\t* (.init_array)\n"
				 "\t\t* (.data)\n"
				 "\t\t* (.text.*)\n"
				 "\t\t* (.rodata.*)\n"
				 "\t\t* (.init_array.*)\n"
				 "\t\t* (.data.*)\n";
			if (hookCount != 0)
			{
				o += "\t\t. = ALIGN(4);\n"
					 "\t\tncp_hookdata = .;\n"
					 "\t\tFILL(0)\n"
					 "\t\t. = ncp_hookdata + ";
				o += std::to_string(hookCount * SizeOfHookBridge);
				o += ";\n";
			}
		}
		else
		{
			for (auto& f : *m_srcFileJobs)
			{
				if (f->region == s->region)
				{
					std::string objPath = fs::relative(f->objFilePath).string();
					static const char* secIncs[] = {
						"text",
						"rodata",
						"init_array",
						"data",
						"text.*",
						"rodata.*",
						"init_array.*",
						"data.*"
					};
					for (auto& secInc : secIncs)
						addSectionInclude(o, objPath, secInc);
				}
			}
			if (hookCount != 0)
			{
				o += "\t\t. = ALIGN(4);\n\t\tncp_hookdata_";
				o += s->memory->name;
				o += " = .;\n\t\tFILL(0)\n\t\t. = ncp_hookdata_";
				o += s->memory->name;
				o += " + ";
				o += std::to_string(hookCount * SizeOfHookBridge);
				o += ";\n";
			}
		}
		o += "\t\t. = ALIGN(4);\n"
			 "\t} > ";
		o += s->memory->name;
		o += " AT > bin\n"

		// BSS
		     "\n\t.";
		o += s->memory->name;
		o += ".bss : ALIGN(4) {\n";
		if (s->dest == -1)
		{
			o += "\t\t* (.bss)\n"
				 "\t\t* (.bss.*)\n";
		}
		else
		{
			for (auto& f : *m_srcFileJobs)
			{
				if (f->region == s->region)
				{
					std::string objPath = fs::relative(f->objFilePath).string();
					addSectionInclude(o, objPath, "bss");
					addSectionInclude(o, objPath, "bss.*");
				}
			}
		}
		o += "\t\t. = ALIGN(4);\n"
			 "\t} > ";
		o += s->memory->name;
		o += " AT > bin\n\n";
	}

	for (auto& p : overPatches)
	{
		o += '\t';
		o += p->info->symbol;
		o += " : { KEEP(* (";
		o += p->info->symbol;
		o += ")) } > ";
		o += p->memory->name;
		o += " AT > bin\n";
	}
	if (!overPatches.empty())
		o += '\n';

	for (auto& p : m_destWithNcpSet)
	{
		o += "\t.ncp_set";
		if (p == -1)
		{
			o += " : { KEEP(* (.ncp_set)) } > ncp_set AT > bin\n";
		}
		else
		{
			o += "_ov";
			o += std::to_string(p);
			o += " : {\n";
			for (auto& j : m_jobsWithNcpSet)
			{
				if (j->region->destination == p)
				{
					o += "\t\t KEEP(\"";
					o += j->objFilePath.string();
					o += "\" (.ncp_set))\n\t"
						 "} > ncp_set AT > bin\n";
				}
			}
		}
	}
	if (!m_destWithNcpSet.empty())
		o += '\n';

	o += "\t/DISCARD/ : {*(.*)}\n"
		 "}\n";

	if (!m_externSymbols.empty())
	{
		o += "\nEXTERN (\n";
		for (auto& e : m_externSymbols)
		{
			o += '\t';
			o += e;
			o += '\n';
		}
		o += ")\n";
	}

	// Output the file
	std::ofstream outputFile(m_ldscriptPath);
	if (!outputFile.is_open())
		throw ncp::file_error(m_ldscriptPath, ncp::file_error::write);
	outputFile.write(o.data(), std::streamsize(o.length()));
	outputFile.close();
}

void PatchMaker::linkElfFile()
{
	Log::out << OLINK << "Linking the ARM binary..." << std::endl;

	fs::current_path(*m_buildDir);

	std::string ccmd;
	ccmd.reserve(64);
	ccmd += BuildConfig::getToolchain();
	ccmd += "gcc -Wl,--gc-sections,-T\"";
	ccmd += fs::relative(m_ldscriptPath).string();
	ccmd += "\"";
	if (!m_target->ldFlags.empty())
		ccmd += ",";
	ccmd += m_target->ldFlags;

	std::ostringstream oss;
	int retcode = Process::start(ccmd.c_str(), &oss);
	if (retcode != 0)
	{
		Log::out << oss.str() << std::endl;
		throw ncp::exception("Could not link the ELF file.");
	}
}

void PatchMaker::gatherInfoFromElf()
{
	Log::info("Getting patches from elf...");

	const Elf32_Ehdr& eh = m_elf->getHeader();
	auto sh_tbl = m_elf->getSectionHeaderTable();
	auto str_tbl = m_elf->getSection<char>(sh_tbl[eh.e_shstrndx]);

	auto parseNcpSetSymbol = [&](std::string_view symbolName, u32 srcAddress, int srcAddressOv){
		std::string_view labelName = symbolName.substr(7);

		std::size_t patchTypeNameEnd = labelName.find('_');
		if (patchTypeNameEnd == std::string::npos)
			return;

		std::string_view patchTypeName = labelName.substr(0, patchTypeNameEnd);
		std::size_t patchType = Util::indexOf(patchTypeName, s_patchTypeNames, sizeof(s_patchTypeNames) / sizeof(char*));
		if (patchType == -1)
		{
			Log::out << OWARN << "Found invalid patch type: " << patchTypeName << std::endl;
			return;
		}

		if (patchType != PatchType::Jump && patchType != PatchType::Call && patchType != PatchType::Hook)
		{
			Log::out << OWARN << "\"set\" patch type must be a jump, call or hook: " << patchTypeName << std::endl;
			return;
		}

		bool expectingOverlay = true;
		std::size_t addressNameStart = patchTypeNameEnd + 1;
		std::size_t addressNameEnd = labelName.find('_', addressNameStart);
		if (addressNameEnd == std::string::npos)
		{
			addressNameEnd = labelName.length();
			expectingOverlay = false;
		}
		std::string_view addressName = labelName.substr(addressNameStart, addressNameEnd - addressNameStart);
		u32 destAddress;
		try {
			destAddress = Util::addrToInt(std::string(addressName));
		} catch (std::exception& e) {
			Log::out << OWARN << "Found invalid address for patch: " << labelName << std::endl;
			return;
		}

		int destAddressOv = -1;
		if (expectingOverlay)
		{
			std::size_t overlayNameStart = addressNameEnd + 1;
			std::size_t overlayNameEnd = labelName.length();
			std::string_view overlayName = labelName.substr(overlayNameStart, overlayNameEnd - overlayNameStart);
			if (!overlayName.starts_with("ov"))
			{
				Log::out << OWARN << "Expected overlay definition in patch for: " << labelName << std::endl;
				return;
			}
			try {
				destAddressOv = Util::addrToInt(std::string(overlayName.substr(2)));
			} catch (std::exception& e) {
				Log::out << OWARN << "Found invalid overlay for patch: " << labelName << std::endl;
				return;
			}
		}

		auto* patchInfoEntry = new GenericPatchInfo({
			.srcAddress = srcAddress,
			.srcAddressOv = srcAddressOv,
			.destAddress = destAddress,
			.destAddressOv = destAddressOv,
			.patchType = patchType,
			.sectionIdx = -1,
			.sectionSize = 0,
			.symbol = std::string(symbolName),
			.job = nullptr
		});

		m_patchInfo.emplace_back(patchInfoEntry);
	};

	// Update the patch info with new values
	forEachElfSymbol(*m_elf, eh, sh_tbl,
	[&](const Elf32_Sym& symbol, std::string_view symbolName){
		for (auto& p : m_patchInfo)
		{
			if (p->sectionIdx != -1) // patch is section
			{
				std::string_view nameAsLabel = std::string_view(p->symbol).substr(1);
				if (nameAsLabel == symbolName)
				{
					p->srcAddress = symbol.st_value;
					p->sectionIdx = symbol.st_shndx;
					p->symbol = nameAsLabel;
				}
			}
			else
			{
				// This must run before fetching ncp_set section, otherwise ncp_set srcAddr will be overwritten
				if (p->symbol == symbolName)
				{
					p->srcAddress = symbol.st_value;
					p->sectionIdx = symbol.st_shndx;
				}
			}
		}
		if (symbolName.starts_with("ncp_hookdata"))
		{
			int srcAddrOv = -1;
			if (symbolName.length() != 12 && symbolName.substr(12).starts_with("_ov"))
			{
				try {
					srcAddrOv = std::stoi(std::string(symbolName.substr(15)));
				} catch (std::exception& e) {
					Log::out << OWARN << "Found invalid overlay parsing ncp_hookdata symbol: " << symbolName << std::endl;
					return false;
				}
			}
			auto* info = new HookMakerInfo();
			info->address = symbol.st_value;
			info->curAddress = symbol.st_value;
			m_hookMakerInfoForDest.emplace(srcAddrOv, info);
		}
		return false;
	});

	forEachElfSection(eh, sh_tbl, str_tbl,
	[&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
		for (auto& p : m_patchInfo)
		{
			if (p->patchType == PatchType::Over)
			{
				if (p->symbol == sectionName)
				{
					p->srcAddress = section.sh_addr; // should be the same as the destination
					p->sectionIdx = int(sectionIdx);
				}
			}
		}
		if (sectionName.starts_with(".ncp_set"))
		{
			// found the ncp_set section, get all hook definitions stored there

			int srcAddrOv = -1;
			if (sectionName.length() != 8 && sectionName.substr(8).starts_with("_ov"))
			{
				try {
					srcAddrOv = std::stoi(std::string(sectionName.substr(11)));
				} catch (std::exception& e) {
					Log::out << OWARN << "Found invalid overlay reading ncp_set section: " << sectionName << std::endl;
					return false;
				}
			}

			const char* sectionData = m_elf->getSection<char>(section);

			forEachElfSymbol(*m_elf, eh, sh_tbl,
			[&](const Elf32_Sym& symbol, std::string_view symbolName){
				if (symbolName.starts_with("ncp_set") && symbol.st_shndx == sectionIdx)
				{
					u32 srcAddr = *reinterpret_cast<const u32*>(&sectionData[symbol.st_value - section.sh_addr]);
					parseNcpSetSymbol(symbolName, srcAddr, srcAddrOv);
				}
				return false;
			});
		}
		return false;
	});

	// Check if any overlapping patches exist
	bool foundOverlapping = false;
	for (std::size_t i = 0; i < m_patchInfo.size(); i++)
	{
		auto& a = m_patchInfo[i];
        for (std::size_t j = i + 1; j < m_patchInfo.size(); j++)
		{
			auto& b = m_patchInfo[j];
			if (a->destAddressOv == b->destAddressOv)
			{
				u32 aSz = a->patchType == PatchType::Over ? a->sectionSize : 4;
				u32 bSz = b->patchType == PatchType::Over ? b->sectionSize : 4;
				if (Util::overlaps(a->destAddress, a->destAddress + aSz, b->destAddress, b->destAddress + bSz))
				{
					Log::out << OERROR
						<< OSTRa(a->symbol) << "[sz=" << aSz << "] (" << OSTR(a->job->srcFilePath.string()) << ") overlaps with "
						<< OSTRa(b->symbol) << "[sz=" << bSz << "] (" << OSTR(b->job->srcFilePath.string()) << ")\n";
					foundOverlapping = true;
				}
			}
        }
    }
	if (foundOverlapping)
		throw ncp::exception("Overlapping patches were detected.");
	
	if (Main::getVerbose())
	{
		Log::out << "Patches:\nSRC_ADDR, SRC_ADDR_OV, DST_ADDR, DST_ADDR_OV, PATCH_TYPE, SECTION_IDX, SECTION_SIZE, SYMBOL" << std::endl;
		for (auto& p : m_patchInfo)
		{
			Log::out <<
				std::setw(8) << std::hex << p->srcAddress << "  " <<
				std::setw(11) << std::dec << p->srcAddressOv << "  " <<
				std::setw(8) << std::hex << p->destAddress << "  " <<
				std::setw(11) << std::dec << p->destAddressOv << "  " <<
				std::setw(10) << s_patchTypeNames[p->patchType] << "  " <<
				std::setw(11) << std::dec << p->sectionIdx << "  " <<
				std::setw(12) << std::dec << p->sectionSize << "  " <<
				std::setw(6) << p->symbol << std::endl;
		}
	}

	forEachElfSection(eh, sh_tbl, str_tbl,
	[&](std::size_t sectionIdx, const Elf32_Shdr& section, std::string_view sectionName){
		auto insertSection = [&](int dest, bool isBss){
			auto& newcodeInfo = m_newcodeDataForDest[dest];
			if (newcodeInfo == nullptr)
			    newcodeInfo = std::make_unique<NewcodePatch>();

			(isBss ? newcodeInfo->bssData : newcodeInfo->binData) = m_elf->getSection<u8>(section);
			(isBss ? newcodeInfo->bssSize : newcodeInfo->binSize) = section.sh_size;
			(isBss ? newcodeInfo->bssAlign : newcodeInfo->binAlign) = section.sh_addralign;
		};

		if (sectionName.starts_with(".arm"))
		{
			insertSection(-1, sectionName.substr(5) == "bss");
		}
		else if (sectionName.starts_with(".ov"))
		{
			std::size_t pos = sectionName.find('.', 3);
			if (pos != std::string::npos)
			{
				int dest = std::stoi(std::string(sectionName.substr(3, pos - 3)));
				insertSection(dest, sectionName.substr(pos + 1) == "bss");
			}
		}
		return false;
	});
}

void PatchMaker::loadElfFile()
{
	if (!std::filesystem::exists(m_elfPath))
		throw ncp::file_error(m_elfPath, ncp::file_error::find);

	m_elf = std::make_unique<Elf32>();
	if (!m_elf->load(m_elfPath))
		throw ncp::file_error(m_elfPath, ncp::file_error::read);
}

void PatchMaker::unloadElfFile()
{
	m_elf = nullptr;
}

u32 PatchMaker::makeJumpOpCode(u32 opCode, u32 fromAddr, u32 toAddr)
{
	return opCode | ((((toAddr - fromAddr) >> 2) - 2) & 0xFFFFFF);
}

u32 PatchMaker::fixupOpCode(u32 opCode, u32 ogAddr, u32 newAddr)
{
	if (((opCode >> 25) & 0b111) == 0b101)
	{
		u32 opCodeBase = opCode & 0xFF000000;
		u32 toAddr = (((opCode & 0xFFFFFF) + 2) << 2) + ogAddr;
		return makeJumpOpCode(opCodeBase, newAddr, toAddr);
	}
	return opCode;
}

void PatchMaker::applyPatchesToRom()
{
	Main::setErrorContext(m_target->getArm9() ?
		"Failed to apply patches for ARM9 target." :
		"Failed to apply patches for ARM7 target.");

	Log::info("Patching the binaries...");

	const u32 armOpcodeB = 0xEA000000; // B
	const u32 armOpcodeBL = 0xEB000000; // BL
	//const u32 armHookPush = 0xE92D100F; // PUSH {R0-R3,R12}
	//const u32 armHookPop = 0xE8BD100F; // POP {R0-R3,R12}
	const u32 armHookPush = 0xE92D500F; // PUSH {R0-R3,R12,LR}
	const u32 armHookPop = 0xE8BD500F; // POP {R0-R3,R12,LR}

	auto sh_tbl = m_elf->getSectionHeaderTable();

	for (auto& p : m_patchInfo)
	{
		ICodeBin* bin = (p->destAddressOv == -1) ?
						static_cast<ICodeBin*>(getArm()) :
						static_cast<ICodeBin*>(getOverlay(p->destAddressOv));

		switch (p->patchType)
		{
		case PatchType::Jump:
		{
			bin->write<u32>(p->destAddress, makeJumpOpCode(armOpcodeB, p->destAddress, p->srcAddress));
			break;
		}
		case PatchType::Call:
		{
			bin->write<u32>(p->destAddress, makeJumpOpCode(armOpcodeBL, p->destAddress, p->srcAddress));
			break;
		}
		case PatchType::Hook:
		{
			/*
			 * If the patch type is a hook, the instruction at
			 * destAddr must become a jump to a hook bridge generated
			 * by NCPatcher and it should look as such:
			 *
			 * hook_bridge:
			 *     PUSH {R0-R3,R12}
			 *     BL   srcAddr
			 *     POP  {R0-R3,R12}
			 *     <unpatched destAddr's instruction>
			 *     B    (destAddr + 4)
			 * */

			u32 ogOpCode = bin->read<u32>(p->destAddress);

			auto& info = m_hookMakerInfoForDest[p->srcAddressOv];
			if (info == nullptr)
				throw ncp::exception("Unexpected p->srcAddressOv for m_hookMakerInfoForDest encountered.");

			std::vector<u8>& hookData = info->data;
			std::size_t offset = hookData.size();
			hookData.resize(offset + SizeOfHookBridge);

			u32 hookBridgeAddr = info->curAddress;

			if (Main::getVerbose())
				Log::out << "HOOK DEST: " << Util::intToAddr(hookBridgeAddr, 8) << std::endl;

			bin->write<u32>(p->destAddress, makeJumpOpCode(armOpcodeB, p->destAddress, hookBridgeAddr));

			u8* hookDataPtr = hookData.data() + offset;

			Util::write<u32>(hookDataPtr, armHookPush);
			Util::write<u32>(hookDataPtr + 4, makeJumpOpCode(armOpcodeBL, hookBridgeAddr + 4, p->srcAddress));
			Util::write<u32>(hookDataPtr + 8, armHookPop);
			Util::write<u32>(hookDataPtr + 12, fixupOpCode(ogOpCode, p->destAddress, hookBridgeAddr + 12));
			Util::write<u32>(hookDataPtr + 16, makeJumpOpCode(armOpcodeB, hookBridgeAddr + 16, p->destAddress + 4));

			if (Main::getVerbose())
				Util::printDataAsHex(hookData.data() + offset, 20, 32);

			info->curAddress += SizeOfHookBridge;
			break;
		}
		case PatchType::Over:
		{
			const char* sectionData = m_elf->getSection<char>(sh_tbl[p->sectionIdx]);
			bin->writeBytes(p->destAddress, sectionData, p->sectionSize);
			break;
		}
		}
	}
	
	for (const auto& [dest, newcodeInfo] : m_newcodeDataForDest)
	{
		u32 newcodeAddr = m_newcodeAddrForDest[dest];

		auto writeNewcode = [&](u8* addr){
			std::size_t hookDataSize = 0;
			auto& hookInfo = m_hookMakerInfoForDest[dest];
			if (hookInfo != nullptr)
				hookDataSize = hookInfo->data.size();

			// Write the patch data
			std::memcpy(addr, newcodeInfo->binData, newcodeInfo->binSize - hookDataSize);
			if (hookDataSize != 0)
				std::memcpy(&addr[newcodeInfo->binSize - hookDataSize], hookInfo->data.data(), hookDataSize);
		};

		if (dest == -1)
		{
			// If more data needs to be added
			if ((newcodeInfo->binSize + newcodeInfo->bssSize) != 0)
			{
				ArmBin* bin = getArm();
				std::vector<u8>& data = bin->data();

				// Extend the ARM binary
				data.resize(data.size() + newcodeInfo->binSize + 12);

				// Write the new relocated code address
				u32 heapReloc = newcodeAddr + newcodeInfo->binSize + (newcodeInfo->bssAlign - newcodeInfo->binSize % newcodeInfo->bssAlign) + newcodeInfo->bssSize;
				bin->write<u32>(m_target->arenaLo, heapReloc);

				ArmBin::ModuleParams* moduleParams = bin->getModuleParams();
				u32 ramAddress = bin->getRamAddress();

				u32 autoloadListStart = moduleParams->autoloadListStart;
				u32 autoloadListEnd = moduleParams->autoloadListEnd;
				u32 binAutoloadListStart = moduleParams->autoloadListStart - ramAddress; // Where our new code will be placed
				u32 binAutoloadListEnd = moduleParams->autoloadListEnd - ramAddress;
				u32 binAutoloadStart = moduleParams->autoloadStart - ramAddress;

				std::vector<ArmBin::AutoLoadEntry>& autoloadList = bin->getAutoloadList();
				autoloadList.insert(autoloadList.begin(), ArmBin::AutoLoadEntry{
					.address = newcodeAddr,
					.size = u32(newcodeInfo->binSize),
					.bssSize = u32(newcodeInfo->bssSize),
					.dataOff = binAutoloadStart
				});

				// Write the new data
				if (newcodeInfo->binSize != 0)
				{
					// Move/offset the old code by the size of our patch
					std::memcpy(&data[binAutoloadStart + newcodeInfo->binSize], &data[binAutoloadStart], binAutoloadListStart - binAutoloadStart);

					writeNewcode(&data[binAutoloadStart]);
				}

				// Set the new autoload list location
				moduleParams->autoloadListStart = autoloadListStart + newcodeInfo->binSize;
				moduleParams->autoloadListEnd = autoloadListEnd + newcodeInfo->binSize + 12;

				// Write the new autoload list after the new code
				u8* writeAutoloadPtr = data.data() + binAutoloadListStart + newcodeInfo->binSize;
				for (ArmBin::AutoLoadEntry& entry : autoloadList)
				{
					u32 entryData[3];
					entryData[0] = entry.address;
					entryData[1] = entry.size;
					entryData[2] = entry.bssSize;
					std::memcpy(writeAutoloadPtr, entryData, 12);
					writeAutoloadPtr += 12;
				}
			}
		}
		else
		{
			const BuildTarget::Region* region = nullptr;
			for (const BuildTarget::Region& r : m_target->regions)
			{
				if (r.destination == dest)
				{
					region = &r;
					break;
				}
			}
			if (region == nullptr)
				throw ncp::exception("region of overlay " + std::to_string(dest) + " set to add code could not be found!");

			switch (region->mode)
			{
			case BuildTarget::Mode::Append:
			{
				OverlayBin* bin = getOverlay(dest);
				auto& ovtEntry = m_ovtEntries[dest];

				ovtEntry->compressed = 0; // size of compressed "ramSize"
				ovtEntry->flag = 0;

				std::vector<u8>& data = bin->data();
				std::size_t szData = data.size();

				std::size_t totalOvSize = szData + ovtEntry->bssSize + newcodeInfo->binSize + newcodeInfo->bssSize;
				if (totalOvSize > region->length)
				{
					throw ncp::exception("Overlay " + std::to_string(dest) + " exceeds max length of "
						+ std::to_string(region->length) + " bytes, got " + std::to_string(totalOvSize) + " bytes.");
				}

				if (newcodeInfo->binSize > 0)
				{
					std::size_t newSzData = szData + ovtEntry->bssSize + newcodeInfo->binSize;
					data.resize(newSzData);
					u8* pData = data.data();
					std::memset(&pData[szData], 0, ovtEntry->bssSize); // Keep original BSS as data
					writeNewcode(&pData[szData + ovtEntry->bssSize]); // Write new code after BSS
					ovtEntry->ramSize = newSzData;
					ovtEntry->bssSize = newcodeInfo->bssSize; // Set the BSS to our new code BSS
				}
				else
				{
					ovtEntry->bssSize += newcodeInfo->bssSize;
				}

				break;
			}
			case BuildTarget::Mode::Replace:
			{
				OverlayBin* bin = getOverlay(dest);
				auto& ovtEntry = m_ovtEntries[dest];

				ovtEntry->ramAddress = newcodeAddr;
				ovtEntry->ramSize = newcodeInfo->binSize;
				ovtEntry->bssSize = newcodeInfo->bssSize;
				ovtEntry->sinitStart = 0;
				ovtEntry->sinitEnd = 0;
				ovtEntry->compressed = 0; // size of compressed "ramSize"
				ovtEntry->flag = 0;

				std::size_t totalOvSize = newcodeInfo->binSize + newcodeInfo->bssSize;
				if (totalOvSize > region->length)
				{
					throw ncp::exception("Overlay " + std::to_string(dest) + " exceeds max length of "
						+ std::to_string(region->length) + " bytes, got " + std::to_string(totalOvSize) + " bytes.");
				}

				std::vector<u8>& data = bin->data();
				
				// Write the new data
				if (newcodeInfo->binSize == 0)
				{
					data.clear();
				}
				else
				{
					data.resize(newcodeInfo->binSize);
					writeNewcode(data.data());
				}
				break;
			}
			case BuildTarget::Mode::Create:
			{
				// TO BE DESIGNED.
				throw ncp::exception("Creating new overlays is not yet supported.");
				break;
			}
			}
		}
	}

	Main::setErrorContext(nullptr);
}
