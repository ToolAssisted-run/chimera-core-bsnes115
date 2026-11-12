/* bsnes.cpp - bsnes v115 behind Chimera's guest ABI.
 *
 * The frontend half of bsnes - what upstream's own window (target-bsnes)
 * and its libretro core (target-libretro) do around the emulator - written
 * for a sandbox: the game, its firmware and the settings arrive as mounted
 * files, a frame is one call to Emulator::Interface::run(), and nothing is
 * read from or written to a disk.
 *
 * How a cartridge is recognised is upstream's desktop recipe: its game
 * database first (Database/Super Famicom.bml, by the SHA-256 of the ROM),
 * then its heuristics; the per-game adjustments are its hackCompatibility.
 *
 * This file compiles the same for the guest (miniBox's emulibc) and for the
 * native reference build (native-shim/emulibc.h), which is what makes the
 * equivalence gate a real proof.
 */
#include <sfc/sfc.hpp>
#include <nall/decode/zip.hpp>
#include <nall/hash/sha256.hpp>
using namespace nall;

#include <heuristics/heuristics.hpp>
#include <heuristics/heuristics.cpp>
#include <heuristics/super-famicom.cpp>
#include <heuristics/game-boy.cpp>

/* Boards (the board database) and iplrom (the sound processor's boot ROM) */
#include <target-libretro/resources.hpp>
/* SuperFamicomDatabase: Database/Super Famicom.bml, generated at build time */
#include "database.h"

#include <cstdio>
#include <cstring>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

/* A text a person reads is a key of languages/en.json, each of its
 * arguments after a TEXT_ARG (chimera docs/translations.md). */
#define TEXT_ARG "\x1f"

static char g_loadError[512];
static int g_inited;
static int g_inputRead;

/* ---- the wire format: waterbox.config "input.buttons" order ----
 * 0 Power (hard reset), 1 Reset (soft), then P1..P5 x
 * {Up,Down,Left,Right,Select,Start,Y,B,X,A,L,R} (the order Chimera's other
 * SNES core uses), then the devices: port 1's mouse Left/Right (62..63),
 * port 2's mouse Left/Right (64..65), Super Scope Trigger/Cursor/Turbo/Pause
 * (66..69), Justifier 1 Trigger/Start (70..71), Justifier 2 Trigger/Start
 * (72..73). Which blocks are live follows the port settings.
 * Axes (SetAxis), each a movement for this frame, as bsnes takes them:
 * 0/1 port 1 mouse X/Y, 2/3 port 2 mouse X/Y, 4/5 Super Scope X/Y,
 * 6/7 Justifier 1 X/Y, 8/9 Justifier 2 X/Y. */
#define BTN_PADS 2
#define BTN_PER_PAD 12
#define PAD_COUNT 5
#define BTN_MOUSE1 (BTN_PADS + PAD_COUNT * BTN_PER_PAD) /* 62 */
#define BTN_MOUSE2 (BTN_MOUSE1 + 2)                     /* 64 */
#define BTN_SCOPE (BTN_MOUSE2 + 2)                      /* 66 */
#define BTN_JUSTIFIER1 (BTN_SCOPE + 4)                  /* 70 */
#define BTN_JUSTIFIER2 (BTN_JUSTIFIER1 + 2)             /* 72 */
#define BTN_COUNT (BTN_JUSTIFIER2 + 2)                  /* 74 */
#define AXIS_COUNT 10
static uint8_t g_buttons[BTN_COUNT];
static int32_t g_axes[AXIS_COUNT];

/* wire column -> bsnes's gamepad input id (Up 0, Down 1, Left 2, Right 3,
 * B 4, A 5, Y 6, X 7, L 8, R 9, Select 10, Start 11) */
static const uint kColToInput[BTN_PER_PAD] = { 0, 1, 2, 3, 10, 11, 6, 4, 7, 5, 8, 9 };
/* and back */
static int g_inputToCol[BTN_PER_PAD];

static uint g_port1 = SuperFamicom::ID::Device::Gamepad;
static uint g_port2 = SuperFamicom::ID::Device::Gamepad;

/* ---- video: xRGB1555 -> opaque BGRA ---- */
#define VIDEO_MAX_W 512
#define VIDEO_MAX_H 480
static uint32_t g_videoOut[VIDEO_MAX_W * VIDEO_MAX_H];
static int g_vwidth = 256, g_vheight = 224;
static bool g_overscan;
/* Turbo: while off, the picture is not converted. The emulator itself draws
 * either way, so the machine is the same machine; ECL_INVISIBLE because it
 * is the frontend's choice for the moment, not part of the machine. */
ECL_INVISIBLE static int g_render = 1;

/* ---- audio: what Emulator::audio mixed this frame ---- */
#define AUDIO_RATE 44100
#define MAX_SAMPLES 4096
static int16_t g_soundbuffer[MAX_SAMPLES * 2];
static int g_nsamples;

static Emulator::Interface *emulator;

/* A file bsnes writes (save data at export time): kept in guest memory. */
struct CaptureFile : vfs::file
{
	vector<uint8_t> *buffer;
	uintmax position = 0;
	explicit CaptureFile(vector<uint8_t> *b) : buffer(b) { buffer->reset(); }
	auto size() const -> uintmax override { return buffer->size(); }
	auto offset() const -> uintmax override { return position; }
	auto seek(intmax o, index mode) -> void override
	{
		if (mode == index::absolute) position = o;
		else position += o;
	}
	auto read() -> uint8_t override { return position < buffer->size() ? (*buffer)[position++] : 0; }
	auto write(uint8_t data) -> void override
	{
		while (buffer->size() <= position) buffer->append(0);
		(*buffer)[position++] = data;
	}
};

struct SaveFile
{
	string name;
	vector<uint8_t> data;
};
static vector<SaveFile> g_saveFiles;
static bool g_capturing;

static auto readMounted(const char *name) -> vector<uint8_t>
{
	vector<uint8_t> out;
	FILE *f = fopen(name, "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size > 0)
	{
		out.resize(size);
		if (fread(out.data(), 1, size, f) != (size_t)size) out.reset();
	}
	fclose(f);
	return out;
}

/* the first file of a slot, by its mounted name; "" when the slot is empty */
static auto slotFile(const char *slot, uint index = 0) -> string
{
	char name[512];
	if (wbx_slot_count(slot) > (int)index && wbx_slot_name(slot, index, name, sizeof name) != NULL)
		return name;
	return "";
}

static auto lowerSuffix(string name) -> string
{
	return Location::suffix(name).downcase();
}

/* A cartridge file, or the cartridge inside a zip (upstream's loadFile). */
static auto loadGameFile(const string &name) -> vector<uint8_t>
{
	auto data = readMounted(name);
	if (lowerSuffix(name) == ".zip")
	{
		Decode::ZIP archive;
		if (archive.open(data.data(), data.size()))
		{
			for (auto &file : archive.file)
			{
				auto type = lowerSuffix(file.name);
				if (type == ".sfc" || type == ".smc" || type == ".gb" || type == ".gbc")
					return archive.extract(file);
			}
		}
		return {};
	}
	return data;
}

struct Program : Emulator::Platform
{
	struct SuperFamicomGame
	{
		string location;
		string manifest;
		string title;
		string region;
		Markup::Node document;
		bool verified = false;
		vector<uint8_t> program, data, expansion, firmware;
	} superFamicom;

	struct GameBoyGame
	{
		string location;
		string manifest;
		vector<uint8_t> program;
	} gameBoy;

	string regionOption = "Auto";
	vector<uint8_t> saveRam;

	auto open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file> override;
	auto load(uint id, string name, string type, vector<string> options) -> Emulator::Platform::Load override;
	auto videoFrame(const uint16 *data, uint pitch, uint width, uint height, uint scale) -> void override;
	auto audioFrame(const double *samples, uint channels) -> void override;
	auto inputPoll(uint port, uint device, uint input) -> int16 override;
	auto inputRumble(uint port, uint device, uint input, bool enable) -> void override {}

	auto loadSuperFamicom(vector<uint8_t> rom) -> bool;
	auto loadGameBoy(vector<uint8_t> rom) -> bool;
	auto firmwareFile(string identifier, bool program, uint offset, uint size) -> shared_pointer<vfs::file>;
	auto hackCompatibility() -> void;
};
static Program *program;

static auto memoryFile(const vector<uint8_t> &data) -> shared_pointer<vfs::file>
{
	return vfs::memory::file::open(data.data(), data.size());
}

/* A coprocessor's firmware, mounted under the name upstream's desktop build
 * looks for in its Firmware folder: <identifier>.program.rom and
 * <identifier>.data.rom, or the two in one <identifier>.rom - which is also
 * how the firmware arrives when it is appended to the cartridge's ROM. */
auto Program::firmwareFile(string identifier, bool isProgram, uint offset, uint size) -> shared_pointer<vfs::file>
{
	string id = identifier.downcase();
	auto split = readMounted(string{id, isProgram ? ".program.rom" : ".data.rom"});
	if (split.size()) return vfs::memory::file::open(split.data(), split.size());
	auto whole = readMounted(string{id, ".rom"});
	if (whole.size() >= offset + size) return vfs::memory::file::open(whole.data() + offset, size);
	return {};
}

auto Program::open(uint id, string name, vfs::file::mode mode, bool required) -> shared_pointer<vfs::file>
{
	bool read = mode == vfs::file::mode::read;

	/* save data being exported: everything bsnes writes is kept */
	if (!read)
	{
		if (!g_capturing) return {};
		SaveFile f;
		f.name = id == SuperFamicom::ID::GameBoy ? string{"gameboy.", name} : name;
		g_saveFiles.append(f);
		return shared_pointer<vfs::file>(new CaptureFile(&g_saveFiles.right().data));
	}

	if (id == SuperFamicom::ID::System)
	{
		if (name == "boards.bml") return vfs::memory::file::open(Boards, sizeof(Boards));
		if (name == "ipl.rom") return vfs::memory::file::open(iplrom, sizeof(iplrom));
		return {};
	}

	if (id == SuperFamicom::ID::SuperFamicom)
	{
		auto &game = superFamicom;
		if (name == "manifest.bml") return vfs::memory::file::open(game.manifest.data<uint8_t>(), game.manifest.size());
		if (name == "program.rom") return memoryFile(game.program);
		if (name == "data.rom") return memoryFile(game.data);
		if (name == "expansion.rom") return memoryFile(game.expansion);
		if (name == "save.ram" && !gameBoy.program) return saveRam ? memoryFile(saveRam) : shared_pointer<vfs::file>{};
		/* the Super Game Boy's boot ROM, firmware (patches/0003) */
		if (name == "sgb1.boot.rom" || name == "sgb2.boot.rom")
		{
			auto data = readMounted(name);
			return data ? memoryFile(data) : shared_pointer<vfs::file>{};
		}

		/* firmware appended to the ROM, as upstream's openRomSuperFamicom
		 * splits it; else the firmware file the project mounted */
		auto &fw = game.firmware;
		auto chip = [&](const char *arch, const char *content) -> string {
			if (auto memory = game.document[{"game/board/memory(type=ROM,content=", content, ",architecture=", arch, ")"}])
				return memory["identifier"].text();
			return "";
		};
		if (name == "arm6.program.rom")
		{
			if (fw.size() == 0x28000) return vfs::memory::file::open(&fw[0x00000], 0x20000);
			return firmwareFile(chip("ARM6", "Program"), true, 0x00000, 0x20000);
		}
		if (name == "arm6.data.rom")
		{
			if (fw.size() == 0x28000) return vfs::memory::file::open(&fw[0x20000], 0x08000);
			return firmwareFile(chip("ARM6", "Data"), false, 0x20000, 0x08000);
		}
		if (name == "hg51bs169.data.rom")
		{
			if (fw.size() == 0xc00) return memoryFile(fw);
			return firmwareFile(chip("HG51BS169", "Data"), false, 0, 0xc00);
		}
		if (name == "upd7725.program.rom")
		{
			if (fw.size() == 0x2000) return vfs::memory::file::open(&fw[0x0000], 0x1800);
			return firmwareFile(chip("uPD7725", "Program"), true, 0x0000, 0x1800);
		}
		if (name == "upd7725.data.rom")
		{
			if (fw.size() == 0x2000) return vfs::memory::file::open(&fw[0x1800], 0x0800);
			return firmwareFile(chip("uPD7725", "Data"), false, 0x1800, 0x0800);
		}
		if (name == "upd96050.program.rom")
		{
			if (fw.size() == 0xd000) return vfs::memory::file::open(&fw[0x0000], 0xc000);
			return firmwareFile(chip("uPD96050", "Program"), true, 0x0000, 0xc000);
		}
		if (name == "upd96050.data.rom")
		{
			if (fw.size() == 0xd000) return vfs::memory::file::open(&fw[0xc000], 0x1000);
			return firmwareFile(chip("uPD96050", "Data"), false, 0xc000, 0x1000);
		}

		/* MSU-1: <cartridge stem>.msu and <stem>-<n>.pcm, mounted under those
		 * names by the msu1 slot */
		if (name == "msu1/data.rom")
		{
			auto data = readMounted(string{Location::prefix(game.location), ".msu"});
			return data ? memoryFile(data) : shared_pointer<vfs::file>{};
		}
		if (name.match("msu1/track*.pcm"))
		{
			name.trimLeft("msu1/track", 1L);
			auto data = readMounted(string{Location::prefix(game.location), name});
			return data ? memoryFile(data) : shared_pointer<vfs::file>{};
		}
		return {};
	}

	if (id == SuperFamicom::ID::GameBoy)
	{
		if (name == "manifest.bml") return vfs::memory::file::open(gameBoy.manifest.data<uint8_t>(), gameBoy.manifest.size());
		if (name == "program.rom") return memoryFile(gameBoy.program);
		/* in a Super Game Boy the save is the Game Boy cartridge's */
		if (name == "save.ram") return saveRam ? memoryFile(saveRam) : shared_pointer<vfs::file>{};
		return {};
	}
	return {};
}

auto Program::load(uint id, string name, string type, vector<string> options) -> Emulator::Platform::Load
{
	if (id == SuperFamicom::ID::SuperFamicom) return {id, regionOption};
	if (id == SuperFamicom::ID::GameBoy && gameBoy.program) return {id, ""};
	return {};
}

auto Program::loadSuperFamicom(vector<uint8_t> rom) -> bool
{
	if (rom.size() < 0x8000) return false;
	if ((rom.size() & 0x7fff) == 512)
	{
		/* a copier header */
		memory::move(&rom[0], &rom[512], rom.size() - 512);
		rom.resize(rom.size() - 512);
	}

	auto heuristics = Heuristics::SuperFamicom(rom, superFamicom.location);
	auto sha256 = Hash::SHA256(rom).digest();
	superFamicom.title = heuristics.title();
	superFamicom.region = heuristics.videoRegion();
	string manifest;
	if (auto document = BML::unserialize(string{(const char *)SuperFamicomDatabase, sizeof(SuperFamicomDatabase)}))
	{
		if (auto game = document[{"game(sha256=", sha256, ")"}])
		{
			manifest = BML::serialize(game);
			/* the header's title is not in the database, and the per-game
			 * adjustments go by it (as upstream's desktop build does) */
			manifest.append("  title: ", superFamicom.title, "\n");
			superFamicom.verified = true;
		}
	}
	superFamicom.manifest = manifest ? manifest : heuristics.manifest();

	/* upstream's hackPatchMemory */
	if (superFamicom.title == "Satellaview BS-X" && rom.size() >= 0x100000)
	{
		if (rom[0x4a9b] == 0x10) rom[0x4a9b] = 0x80;
		if (rom[0x4d6d] == 0x10) rom[0x4d6d] = 0x80;
		if (rom[0x4ded] == 0x10) rom[0x4ded] = 0x80;
		if (rom[0x4e9a] == 0x10) rom[0x4e9a] = 0x80;
	}
	superFamicom.document = BML::unserialize(superFamicom.manifest);

	uint offset = 0;
	auto take = [&](vector<uint8_t> &into, uint size) {
		if (!size) return;
		if (offset + size > rom.size()) size = rom.size() > offset ? rom.size() - offset : 0;
		into.resize(size);
		if (size) memory::copy(&into[0], &rom[offset], size);
		offset += size;
	};
	take(superFamicom.program, heuristics.programRomSize());
	take(superFamicom.data, heuristics.dataRomSize());
	take(superFamicom.expansion, heuristics.expansionRomSize());
	take(superFamicom.firmware, heuristics.firmwareRomSize());
	return true;
}

auto Program::loadGameBoy(vector<uint8_t> rom) -> bool
{
	if (rom.size() < 0x4000) return false;
	auto heuristics = Heuristics::GameBoy(rom, gameBoy.location);
	gameBoy.manifest = heuristics.manifest();
	gameBoy.program = rom;
	return true;
}

/* upstream's Program::hackCompatibility, over the project's settings */
auto Program::hackCompatibility() -> void
{
	string entropy = emulator->configuration("Hacks/Entropy");
	bool fastJoypadPolling = false;
	bool fastPPU = emulator->configuration("Hacks/PPU/Fast") == "true";
	bool fastDSP = emulator->configuration("Hacks/DSP/Fast") == "true";
	uint renderCycle = 512;

	auto title = superFamicom.title;
	auto region = superFamicom.region;

	if (title == "Arcades Greatest Hits") fastJoypadPolling = true;
	if (title == "TAIKYOKU-IGO Goliath") fastJoypadPolling = true;
	if (title == "WORLD MASTERS GOLF") fastJoypadPolling = true;
	if (title == "AIR STRIKE PATROL" || title == "DESERT FIGHTER") fastPPU = false;
	if (title == "\xe3\x83\x9e\xe3\x83\xbc\xe3\x83\xb4\xe3\x82\xa7\xe3\x83\xa9\xe3\x82\xb9") fastPPU = false; /* マーヴェラス */
	if (title == "SFC \xe3\x82\xaf\xe3\x83\xac\xe3\x83\xa8\xe3\x83\xb3\xe3\x82\xb7\xe3\x83\xb3\xe3\x83\x81\xe3\x83\xa3\xe3\x83\xb3") fastPPU = false; /* SFC クレヨンシンチャン */
	if (title == "Winter olympics") fastPPU = false;
	if (title == "WORLD CUP STRIKER") fastPPU = false;
	if (title == "KOUSHIEN_2") fastDSP = false;
	if (title == "RENDERING RANGER R2") fastDSP = false;
	if (title == "BUBSY II" && region == "PAL") fastDSP = false;
	if (title == "ADVENTURES OF FRANKEN" && region == "PAL") renderCycle = 32;
	if (title == "FIREPOWER 2000" || title == "SUPER SWIV") renderCycle = 32;
	if (title == "NHL '94" || title == "NHL PROHOCKEY'94") renderCycle = 32;
	if (title == "Sugoro Quest++") renderCycle = 128;

	if (emulator->configuration("Hacks/Hotfixes") == "true")
	{
		if (title == "The Hurricanes") entropy = "None";
		if (title == "\xe3\x83\x8b\xe3\x83\x81\xe3\x83\x96\xe3\x83\x84\xe3\x83\xbb\xe3\x82\xa2\xe3\x83\xbc\xe3\x82\xb1\xe3\x83\xbc\xe3\x83\x89\xe3\x83\xbb\xe3\x82\xaf\xe3\x83\xa9\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9") entropy = "None"; /* ニチブツ・アーケード・クラシックス */
	}

	emulator->configure("Hacks/Entropy", entropy);
	emulator->configure("Hacks/CPU/FastJoypadPolling", fastJoypadPolling);
	emulator->configure("Hacks/PPU/Fast", fastPPU);
	emulator->configure("Hacks/PPU/RenderCycle", renderCycle);
	emulator->configure("Hacks/DSP/Fast", fastDSP);
}

auto Program::videoFrame(const uint16 *data, uint pitch, uint width, uint height, uint scale) -> void
{
	if (!g_render) return;
	pitch >>= 1;
	if (!g_overscan)
	{
		uint multiplier = height / 240;
		data += 8 * multiplier * pitch;
		height -= 16 * multiplier;
	}
	if (width > VIDEO_MAX_W) width = VIDEO_MAX_W;
	if (height > VIDEO_MAX_H) height = VIDEO_MAX_H;
	uint32_t *dst = g_videoOut;
	for (uint y = 0; y < height; y++)
	{
		const uint16 *src = data + y * pitch;
		for (uint x = 0; x < width; x++)
		{
			uint16 c = src[x];
			uint32_t r = c >> 10 & 31, g = c >> 5 & 31, b = c & 31;
			r = r << 3 | r >> 2;
			g = g << 3 | g >> 2;
			b = b << 3 | b >> 2;
			*dst++ = b | g << 8 | r << 16 | 0xff000000u;
		}
	}
	g_vwidth = width;
	g_vheight = height;
}

static int16_t toSample(double v)
{
	v *= 0x8000;
	if (v > 0x7fff) v = 0x7fff;
	else if (v < -0x8000) v = -0x8000;
	return (int16_t)floor(v + 0.5);
}

auto Program::audioFrame(const double *samples, uint channels) -> void
{
	if (g_nsamples >= MAX_SAMPLES) return;
	g_soundbuffer[g_nsamples * 2 + 0] = toSample(samples[0]);
	g_soundbuffer[g_nsamples * 2 + 1] = toSample(channels > 1 ? samples[1] : samples[0]);
	g_nsamples++;
}

/* Chimera's function to call when the machine reads its controllers, so a
 * Lua script's event.oninputpoll runs there (chimera#344). Called once a
 * frame, at the first read. The same callback slot in every session; no
 * savestate carries it. */
ECL_INVISIBLE static uintptr_t (*g_inputPollCallback)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

auto Program::inputPoll(uint port, uint device, uint input) -> int16
{
	using namespace SuperFamicom;
	if (!g_inputRead)
	{
		g_inputRead = 1;
		if (g_inputPollCallback) g_inputPollCallback(0, 0, 0, 0, 0, 0);
	}
	switch (device)
	{
	case ID::Device::Gamepad:
		if (input >= BTN_PER_PAD) return 0;
		/* port 1 is player 1; port 2's pad is player 2 */
		return g_buttons[BTN_PADS + (port == ID::Port::Controller1 ? 0 : 1) * BTN_PER_PAD + g_inputToCol[input]];
	case ID::Device::SuperMultitap:
	{
		/* players 2 to 5 */
		uint player = 1 + input / 12;
		if (player >= PAD_COUNT || input % 12 >= BTN_PER_PAD) return 0;
		return g_buttons[BTN_PADS + player * BTN_PER_PAD + g_inputToCol[input % 12]];
	}
	case ID::Device::Mouse:
	{
		/* X 0, Y 1, Left 2, Right 3 */
		uint p = port == ID::Port::Controller1 ? 0 : 1;
		if (input < 2) return (int16)g_axes[p * 2 + input];
		if (input < 4) return g_buttons[(p ? BTN_MOUSE2 : BTN_MOUSE1) + input - 2];
		return 0;
	}
	case ID::Device::SuperScope:
		/* X 0, Y 1, Trigger 2, Cursor 3, Turbo 4, Pause 5 */
		if (input < 2) return (int16)g_axes[4 + input];
		if (input < 6) return g_buttons[BTN_SCOPE + input - 2];
		return 0;
	case ID::Device::Justifier:
	case ID::Device::Justifiers:
	{
		/* X 0, Y 1, Trigger 2, Start 3; the second gun's are 4 more */
		uint gun = input / 4, which = input % 4;
		if (gun > 1) return 0;
		if (which < 2) return (int16)g_axes[6 + gun * 2 + which];
		return g_buttons[(gun ? BTN_JUSTIFIER2 : BTN_JUSTIFIER1) + which - 2];
	}
	}
	return 0;
}

static auto portDevice(const char *name, bool right) -> uint
{
	using namespace SuperFamicom;
	char buf[32] = "gamepad";
	wbx_setting_str(name, buf, sizeof buf);
	if (!strcmp(buf, "none")) return ID::Device::None;
	if (!strcmp(buf, "gamepad")) return ID::Device::Gamepad;
	if (!strcmp(buf, "mouse")) return ID::Device::Mouse;
	if (right)
	{
		if (!strcmp(buf, "multitap")) return ID::Device::SuperMultitap;
		if (!strcmp(buf, "superScope")) return ID::Device::SuperScope;
		if (!strcmp(buf, "justifier")) return ID::Device::Justifier;
		if (!strcmp(buf, "justifiers")) return ID::Device::Justifiers;
	}
	fprintf(stderr, "chimera-bsnes115: unknown %s '%s', using a gamepad\n", name, buf);
	return ID::Device::Gamepad;
}

static auto settingBool(const char *name, bool fallback) -> bool
{
	return wbx_setting_bool(name, fallback ? 1 : 0) != 0;
}

static auto settingStr(const char *name, const char *fallback) -> string
{
	char buf[64];
	strncpy(buf, fallback, sizeof buf - 1);
	buf[sizeof buf - 1] = 0;
	wbx_setting_str(name, buf, sizeof buf);
	return buf;
}

/* the settings the project chose, under upstream's configuration names */
static void configure()
{
	emulator->configure("Audio/Frequency", AUDIO_RATE);
	emulator->configure("Video/BlurEmulation", false);
	emulator->configure("Hacks/Hotfixes", settingBool("hotfixes", true));
	emulator->configure("Hacks/Entropy", settingStr("entropy", "Low"));
	emulator->configure("Hacks/CPU/Overclock", (uint)wbx_setting_long("cpuOverclock", 100));
	emulator->configure("Hacks/CPU/FastMath", settingBool("cpuFastMath", false));
	emulator->configure("Hacks/PPU/Fast", settingBool("fastPPU", true));
	emulator->configure("Hacks/PPU/Deinterlace", settingBool("deinterlace", true));
	emulator->configure("Hacks/PPU/NoSpriteLimit", settingBool("noSpriteLimit", false));
	emulator->configure("Hacks/PPU/NoVRAMBlocking", settingBool("noVRAMBlocking", false));
	emulator->configure("Hacks/PPU/Mode7/Scale", 1u);
	emulator->configure("Hacks/DSP/Fast", settingBool("fastDSP", true));
	emulator->configure("Hacks/DSP/Cubic", settingBool("cubicInterpolation", false));
	emulator->configure("Hacks/DSP/EchoShadow", settingBool("echoShadow", false));
	emulator->configure("Hacks/Coprocessor/DelayedSync", settingBool("coprocessorDelayedSync", true));
	emulator->configure("Hacks/Coprocessor/PreferHLE", settingBool("coprocessorPreferHLE", false));
	emulator->configure("Hacks/SA1/Overclock", (uint)wbx_setting_long("sa1Overclock", 100));
	emulator->configure("Hacks/SuperFX/Overclock", (uint)wbx_setting_long("superFXOverclock", 100));
	g_overscan = settingBool("showOverscan", false);
	program->regionOption = settingStr("region", "Auto");
}

/* ---- the machine's memory, as Chimera's memory views ---- */
struct Domain
{
	const char *name;
	uint8_t *data;
	int64_t size;
};
static vector<Domain> g_domains;

static void addDomain(const char *name, uint8_t *data, int64_t size)
{
	if (data && size > 0) g_domains.append(Domain{name, data, size});
}

static void buildDomains()
{
	using namespace SuperFamicom;
	g_domains.reset();
	addDomain("WRAM", cpu.wram, sizeof(cpu.wram));
	if (cartridge.has.SA1)
		addDomain("CARTRAM", sa1.bwram.data(), sa1.bwram.size());
	else if (cartridge.has.SuperFX)
		addDomain("CARTRAM", superfx.ram.data(), superfx.ram.size());
	else
		addDomain("CARTRAM", cartridge.ram.data(), cartridge.ram.size());
	if (cartridge.has.SA1) addDomain("SA1 IRAM", sa1.iram.data(), sa1.iram.size());
	if (SuperFamicom::system.fastPPU())
		addDomain("VRAM", (uint8_t *)ppufast.vram, sizeof(ppufast.vram));
	else
		addDomain("VRAM", (uint8_t *)ppu.videoMemory(), 64 * 1024);
	addDomain("APURAM", dsp.apuram, sizeof(dsp.apuram));
	if (cartridge.has.SA1)
		addDomain("CARTROM", sa1.rom.data(), sa1.rom.size());
	else if (cartridge.has.SuperFX)
		addDomain("CARTROM", superfx.rom.data(), superfx.rom.size());
	else
		addDomain("CARTROM", cartridge.rom.data(), cartridge.rom.size());
}

/* The coprocessor firmware a cartridge's board names, as Chimera firmware
 * declarations (the package's "gameFirmware"). The id is the file bsnes is
 * given: <identifier>.rom, the program and data in one, as upstream splits
 * firmware appended to a ROM. bsnes has its own versions of the DSP-1 to 4,
 * the ST010 and the Cx4, which it uses when "Prefer coprocessor HLE" is on;
 * so their firmware is asked for only when it is off. The ST011 and ST018
 * have none: their firmware is always needed. */
struct ChipFirmware
{
	const char *architecture;
	const char *content;
	uint size;
	bool hasHLE;
};
static const ChipFirmware kChips[] = {
	{ "uPD7725", "Program", 0x2000, true },
	{ "uPD96050", "Program", 0xd000, false },
	{ "HG51BS169", "Data", 0xc00, true },
	{ "ARM6", "Program", 0x28000, false },
};

static auto jsonString(const string &text) -> string
{
	string out = "\"";
	for (char c : text)
	{
		if (c == '"' || c == '\\') out.append("\\", string{c});
		else if ((uint8_t)c < 0x20) out.append("\\u00", hex((uint8_t)c, 2L));
		else out.append(string{c});
	}
	return out.append("\"");
}

static auto gameFirmware() -> string
{
	string out = "[";
	auto &game = program->superFamicom;
	for (auto &chip : kChips)
	{
		auto memory = game.document[{"game/board/memory(type=ROM,content=", chip.content, ",architecture=", chip.architecture, ")"}];
		if (!memory) continue;
		/* firmware appended to the ROM needs nothing more */
		if (game.firmware.size() == chip.size) continue;
		/* the database spells it DSP1B, upstream's heuristics dsp1b */
		string identifier = memory["identifier"].text().upcase();
		string file = {identifier.downcase(), ".rom"};
		/* bsnes has its own ST010; the ST011 shares the chip and has none */
		bool hle = chip.hasHLE || identifier == "ST010";
		if (out.size() > 1) out.append(",");
		out.append("{\"id\":", jsonString(file), ",\"display\":", jsonString(identifier),
			",\"name\":", jsonString(file), ",\"size\":", chip.size,
			",\"description\":", jsonString({hle ? "game_firmware.about_hle" : "game_firmware.about", TEXT_ARG, identifier}));
		if (hle) out.append(",\"requiredWhen\":{\"not\":{\"setting\":\"coprocessorPreferHLE\",\"is\":true}}");
		out.append("}");
	}
	return out.append("]");
}

extern "C" {

ECL_EXPORT const char *GetLoadError(void)
{
	return g_loadError;
}

/* For the New Project window and every load: which coprocessor firmware
 * this cartridge needs. Reads the cartridge as Init would, boots nothing. */
static string g_suggestion;
ECL_EXPORT const char *SuggestSettings(void)
{
	emulator = new SuperFamicom::Interface;
	program = new Program;
	g_suggestion = "{\"values\":{},\"firmware\":[]}";
	string game = slotFile("game");
	if (!game) return g_suggestion;
	auto type = lowerSuffix(game);
	if (type == ".gb" || type == ".gbc") return g_suggestion;
	program->superFamicom.location = game;
	if (!program->loadSuperFamicom(loadGameFile(game))) return g_suggestion;
	g_suggestion = {"{\"values\":{},\"firmware\":", gameFirmware(), "}"};
	return g_suggestion;
}

ECL_EXPORT int Init(void)
{
	g_loadError[0] = 0;
	for (uint col = 0; col < BTN_PER_PAD; col++) g_inputToCol[kColToInput[col]] = col;

	emulator = new SuperFamicom::Interface;
	program = new Program;
	platform = program;
	configure();

	/* the game: a cartridge, or a Game Boy cartridge in a Super Game Boy */
	string game = slotFile("game");
	if (!game) game = "rom";
	auto data = loadGameFile(game);
	if (!data)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.not_read" TEXT_ARG "%s", (const char *)game);
		return 0;
	}

	auto type = lowerSuffix(game);
	if (type == ".gb" || type == ".gbc")
	{
		program->gameBoy.location = game;
		if (!program->loadGameBoy(data))
		{
			snprintf(g_loadError, sizeof g_loadError, "error.rejected" TEXT_ARG "%s", (const char *)game);
			return 0;
		}
		/* the Super Game Boy cartridge the project chose: firmware */
		const char *cart = settingStr("superGameBoy", "SGB") == "SGB2" ? "sgb2.sfc" : "sgb1.sfc";
		auto sgb = readMounted(cart);
		if (!sgb)
		{
			snprintf(g_loadError, sizeof g_loadError, "error.no_sgb" TEXT_ARG "%s", cart);
			return 0;
		}
		program->superFamicom.location = cart;
		if (!program->loadSuperFamicom(sgb))
		{
			snprintf(g_loadError, sizeof g_loadError, "error.rejected" TEXT_ARG "%s", cart);
			return 0;
		}
	}
	else
	{
		program->superFamicom.location = game;
		if (!program->loadSuperFamicom(data))
		{
			snprintf(g_loadError, sizeof g_loadError, "error.rejected" TEXT_ARG "%s", (const char *)game);
			return 0;
		}
	}

	/* the save the project starts from, if it has one */
	string save = slotFile("save");
	if (save) program->saveRam = readMounted(save);

	if (!emulator->load())
	{
		snprintf(g_loadError, sizeof g_loadError, "error.rejected" TEXT_ARG "%s", (const char *)game);
		return 0;
	}
	program->hackCompatibility();

	g_port1 = portDevice("leftPort", false);
	g_port2 = portDevice("rightPort", true);
	emulator->connect(SuperFamicom::ID::Port::Controller1, g_port1);
	emulator->connect(SuperFamicom::ID::Port::Controller2, g_port2);

	emulator->power();
	buildDomains();

	/* one greppable line per boot: the gate reads it to know the two builds
	 * saw the same cartridge, picture processor, coprocessor and MSU-1 pack */
	auto &has = SuperFamicom::cartridge.has;
	const char *chip = has.NECDSP || has.HitachiDSP || has.ARMDSP ? "a coprocessor from its firmware"
		: has.DSP1 || has.DSP2 || has.DSP4 || has.Cx4 || has.ST0010 ? "a coprocessor of bsnes's own"
		: "no firmware coprocessor";
	fprintf(stderr, "[bsnes115] %s cartridge \"%s\" (%s), %s, %s, MSU1 %s\n",
		program->superFamicom.verified ? "verified" : "unverified",
		(const char *)program->superFamicom.title, (const char *)program->superFamicom.region,
		SuperFamicom::system.fastPPU() ? "fast PPU" : "accurate PPU", chip,
		has.MSU1 ? "present" : "absent");

	g_inited = 1;
	return 1;
}

ECL_EXPORT void SetButton(int32_t index, int32_t state)
{
	if (index >= 0 && index < BTN_COUNT) g_buttons[index] = state != 0;
}

ECL_EXPORT void SetAxis(int32_t index, int32_t value)
{
	if (index >= 0 && index < AXIS_COUNT) g_axes[index] = value;
}

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	(void)packed; /* more than 64 buttons: everything rides SetButton */
	if (!g_inited) return;

	/* Power is the console's power switch turned off and on; Reset its
	 * reset button */
	if (g_buttons[0])
	{
		emulator->power();
		buildDomains();
	}
	else if (g_buttons[1])
		emulator->reset();

	g_inputRead = 0;
	g_nsamples = 0;
	emulator->run();
}

ECL_EXPORT void SetRenderingEnabled(int on) { g_render = on != 0; }

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_videoOut; }
ECL_EXPORT int GetVideoWidth(void) { return g_vwidth; }
ECL_EXPORT int GetVideoHeight(void) { return g_vheight; }

ECL_EXPORT int16_t *GetAudio(void) { return g_soundbuffer; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_nsamples; }

ECL_EXPORT int GetVsyncNumerator(void)
{
	return SuperFamicom::system.region() == SuperFamicom::System::Region::PAL ? 21281370 : 21477272;
}
ECL_EXPORT int GetVsyncDenominator(void)
{
	return SuperFamicom::system.region() == SuperFamicom::System::Region::PAL ? 425568 : 357366;
}

ECL_EXPORT int InputWasRead(void) { return g_inputRead; }

ECL_EXPORT void SetInputPollCallback(uint64_t fn)
{
	g_inputPollCallback = (uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(uintptr_t)fn;
}

ECL_EXPORT int GetMemoryDomainCount(void) { return g_domains.size(); }
ECL_EXPORT const char *GetMemoryDomainName(int i) { return i >= 0 && i < (int)g_domains.size() ? g_domains[i].name : NULL; }
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i) { return i >= 0 && i < (int)g_domains.size() ? g_domains[i].data : NULL; }
ECL_EXPORT int64_t GetMemoryDomainSize(int i) { return i >= 0 && i < (int)g_domains.size() ? g_domains[i].size : 0; }
ECL_EXPORT int GetMemoryDomainWritable(int i) { return i >= 0 && i < (int)g_domains.size() && strcmp(g_domains[i].name, "CARTROM") != 0; }

/* ---- save data: what bsnes would write to its Saves folder, asked for
 * when the person exports it. bsnes's save() only reads the machine. ---- */
ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
	g_saveFiles.reset();
	if (!g_inited) return 0;
	g_capturing = true;
	emulator->save();
	g_capturing = false;
	/* drop what came out empty */
	vector<SaveFile> kept;
	for (auto &f : g_saveFiles)
		if (f.data.size()) kept.append(f);
	g_saveFiles = kept;
	return g_saveFiles.size();
}
ECL_EXPORT const char *GetSaveDataFileName(int32_t i) { return i >= 0 && i < (int32_t)g_saveFiles.size() ? (const char *)g_saveFiles[i].name : NULL; }
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i) { return i >= 0 && i < (int32_t)g_saveFiles.size() ? (int64_t)g_saveFiles[i].data.size() : 0; }
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i) { return i >= 0 && i < (int32_t)g_saveFiles.size() ? g_saveFiles[i].data.data() : NULL; }

} /* extern "C" */
