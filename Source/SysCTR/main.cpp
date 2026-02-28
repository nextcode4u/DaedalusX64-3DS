#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include <3ds.h>
#include <GL/picaGL.h>

#include "BuildOptions.h"
#include "Config/ConfigOptions.h"
#include "Core/Cheats.h"
#include "Core/CPU.h"
#include "Core/CPU.h"
#include "Core/Memory.h"
#include "Core/PIF.h"
#include "Core/RomSettings.h"
#include "Core/Save.h"
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "Graphics/GraphicsContext.h"
#include "HLEGraphics/TextureCache.h"
#include "Input/InputManager.h"
#include "Interface/RomDB.h"
#include "System/Paths.h"
#include "System/System.h"
#include "Test/BatchTest.h"


#include "Utility/IO.h"
#include "Utility/Preferences.h"
#include "Utility/Profiler.h"
#include "Utility/Thread.h"
#include "Utility/Translate.h"
#include "Utility/Timer.h"
#include "Utility/ROMFile.h"
#include "Utility/MemoryCTR.h"

bool isN3DS = false;
bool shouldQuit = false;

EAudioPluginMode enable_audio = APM_ENABLED_ASYNC;

namespace UI
{
	void DrawInGameMenu() {}
}

static bool ReadLaunchPathfile(const char * pathfile, char * out_path, size_t out_size)
{
	if (out_path == nullptr || out_size == 0)
	{
		return false;
	}

	FILE * file = fopen(pathfile, "rb");
	if (file == nullptr)
	{
		return false;
	}

	size_t bytes_read = fread(out_path, 1, out_size - 1, file);
	fclose(file);
	out_path[bytes_read] = '\0';

	while (bytes_read > 0)
	{
		const char c = out_path[bytes_read - 1];
		if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
		{
			out_path[bytes_read - 1] = '\0';
			--bytes_read;
			continue;
		}
		break;
	}

	size_t start = 0;
	while (out_path[start] == ' ' || out_path[start] == '\t' || out_path[start] == '\n' || out_path[start] == '\r')
	{
		++start;
	}
	if (start > 0)
	{
		memmove(out_path, out_path + start, strlen(out_path + start) + 1);
	}

	return out_path[0] != '\0';
}

static bool FileExists(const char * path)
{
	if (path == nullptr || path[0] == '\0')
		return false;
	FILE * f = fopen(path, "rb");
	if (f == nullptr)
		return false;
	fclose(f);
	return true;
}

static bool HasRomExtension(const std::string & name)
{
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos)
		return false;

	std::string ext = name.substr(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return ext == "z64" || ext == "n64" || ext == "v64" || ext == "zip";
}

static void EnsureTrailingSlash(std::string & path)
{
	if (!path.empty() && path[path.size() - 1] != '/')
		path.push_back('/');
}

static void GoToParentDir(std::string & path)
{
	EnsureTrailingSlash(path);
	if (path == "sdmc:/")
		return;

	if (!path.empty() && path[path.size() - 1] == '/')
		path.pop_back();

	size_t slash = path.find_last_of('/');
	if (slash == std::string::npos || slash < 5)
	{
		path = "sdmc:/";
		return;
	}
	path.erase(slash + 1);
}

static bool SelectRomFromSdmcBrowser(char * out_fullpath, size_t out_size)
{
	if (out_fullpath == nullptr || out_size == 0)
		return false;

	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);

	std::string current_dir = "sdmc:/";
	int selected = 0;
	int top = 0;
	const int page_size = 14;
	const int repeat_initial = 16;
	const int repeat_interval = 3;
	int repeat_counter = 0;
	int repeat_direction = 0;
	bool dir_dirty = true;
	bool ui_dirty = true;
	std::vector<std::pair<std::string, bool> > entries;

	auto RefreshEntries = [&]() {
		std::vector<std::string> dirs;
		std::vector<std::string> files;

		DIR * dir = opendir(current_dir.c_str());
		if (dir != nullptr)
		{
			struct dirent * ent = nullptr;
			while ((ent = readdir(dir)) != nullptr)
			{
				const char * name = ent->d_name;
				if (name[0] == '.')
					continue;

				std::string entry(name);
				if (ent->d_type == DT_DIR)
				{
					dirs.push_back(entry);
				}
				else if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN)
				{
					if (HasRomExtension(entry))
						files.push_back(entry);
				}
			}
			closedir(dir);
		}

		std::sort(dirs.begin(), dirs.end());
		std::sort(files.begin(), files.end());

		entries.clear();
		if (current_dir != "sdmc:/")
			entries.push_back(std::make_pair(std::string(".."), true));
		for (size_t i = 0; i < dirs.size(); ++i)
			entries.push_back(std::make_pair(dirs[i], true));
		for (size_t i = 0; i < files.size(); ++i)
			entries.push_back(std::make_pair(files[i], false));
	};

	while (aptMainLoop())
	{
		if (dir_dirty)
		{
			RefreshEntries();
			dir_dirty = false;
			ui_dirty = true;
		}

		if (entries.empty())
			selected = 0;
		else if (selected >= (int)entries.size())
			selected = (int)entries.size() - 1;
		if (selected < 0)
			selected = 0;

		if (selected < top)
			top = selected;
		if (selected >= top + page_size)
			top = selected - page_size + 1;
		if (top < 0)
			top = 0;

		if (ui_dirty)
		{
			consoleClear();
			printf("DaedalusX64 Browser\n");
			printf("A: Select  B: Up  START: Exit\n");
			printf("Hold UP/DOWN to scroll faster\n");
			printf("%s\n\n", current_dir.c_str());

			for (int i = 0; i < page_size; ++i)
			{
				const int idx = top + i;
				if (idx >= (int)entries.size())
					break;
				const bool is_dir = entries[idx].second;
				const char * prefix = (idx == selected) ? ">" : " ";
				if (is_dir)
					printf("%s [%s]\n", prefix, entries[idx].first.c_str());
				else
					printf("%s %s\n", prefix, entries[idx].first.c_str());
			}

			gfxFlushBuffers();
			gfxSwapBuffers();
			ui_dirty = false;
		}

		hidScanInput();
		const u32 down = hidKeysDown();
		const u32 held = hidKeysHeld();
		int move = 0;

		if (down & KEY_DOWN)
		{
			move = 1;
			repeat_direction = 1;
			repeat_counter = repeat_initial;
		}
		else if (down & KEY_UP)
		{
			move = -1;
			repeat_direction = -1;
			repeat_counter = repeat_initial;
		}
		else if ((held & KEY_DOWN) && repeat_direction == 1)
		{
			if (--repeat_counter <= 0)
			{
				move = 1;
				repeat_counter = repeat_interval;
			}
		}
		else if ((held & KEY_UP) && repeat_direction == -1)
		{
			if (--repeat_counter <= 0)
			{
				move = -1;
				repeat_counter = repeat_interval;
			}
		}
		else
		{
			repeat_direction = 0;
			repeat_counter = 0;
		}

		if (move != 0)
		{
			if (!entries.empty())
				selected = (selected + move + (int)entries.size()) % (int)entries.size();
			ui_dirty = true;
		}
		else if (down & KEY_B)
		{
			GoToParentDir(current_dir);
			selected = 0;
			top = 0;
			dir_dirty = true;
		}
		else if (down & KEY_A)
		{
			if (!entries.empty())
			{
				const std::string name = entries[selected].first;
				const bool is_dir = entries[selected].second;
				if (is_dir)
				{
					if (name == "..")
					{
						GoToParentDir(current_dir);
					}
					else
					{
						EnsureTrailingSlash(current_dir);
						current_dir += name;
						EnsureTrailingSlash(current_dir);
					}
					selected = 0;
					top = 0;
					dir_dirty = true;
				}
				else
				{
					std::string full = current_dir;
					EnsureTrailingSlash(full);
					full += name;
					snprintf(out_fullpath, out_size, "%s", full.c_str());
					gfxExit();
					return true;
				}
			}
		}
		else if (down & KEY_START)
		{
			gfxExit();
			return false;
		}

		gspWaitForVBlank();
	}

	gfxExit();
	return false;
}

static void SetBottomBacklightEnabled(bool enabled)
{
	if (R_FAILED(gspLcdInit()))
		return;

	if (enabled)
		GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
	else
		GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);

	gspLcdExit();
}

#ifdef DAEDALUS_LOG
void log2file(const char *format, ...) {
	__gnuc_va_list arg;
	int done;
	va_start(arg, format);
	char msg[512];
	done = vsprintf(msg, format, arg);
	va_end(arg);
	sprintf(msg, "%s\n", msg);
	FILE *log = fopen("sdmc:/DaedalusX64.log", "a+");
	if (log != NULL) {
		fwrite(msg, 1, strlen(msg), log);
		fclose(log);
	}
}
#endif

static void CheckDSPFirmware()
{
	FILE *firmware = fopen("sdmc:/3ds/dspfirm.cdc", "rb");

	if(firmware != NULL)
	{
		fclose(firmware);
		return;
	}

	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);

	printf("DSP Firmware not found!\n\n");
	printf("Press START to exit\n");

	while(aptMainLoop())
	{
		hidScanInput();

		if(hidKeysDown() == KEY_START)
			exit(1);
	}
}

static void Initialize()
{
	CheckDSPFirmware();
	
	_InitializeSvcHack();

	romfsInit();
	
	APT_CheckNew3DS(&isN3DS);
	osSetSpeedupEnable(true);
	
	gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, true);

	if(isN3DS)
		gfxSetWide(true);
	
	pglInit();

	strcpy(gDaedalusExePath, DAEDALUS_CTR_PATH(""));
	strcpy(g_DaedalusConfig.mSaveDir, DAEDALUS_CTR_PATH("SaveGames/"));

	IO::Directory::EnsureExists( DAEDALUS_CTR_PATH("SaveStates/") );

	System_Init();
}


void HandleEndOfFrame()
{
	shouldQuit = !aptMainLoop();
	
	if (shouldQuit)
	{
		CPU_Halt("Exiting");
	}
}

extern u32 __ctru_heap_size;

int main(int argc, char* argv[])
{
	char fullpath[512] = { 0 };
	char pathfile_rom[512] = { 0 };
	const bool has_valid_pathfile_rom =
		ReadLaunchPathfile("sdmc:/pathfile/n64_launch.txt", pathfile_rom, sizeof(pathfile_rom)) &&
		FileExists(pathfile_rom);

	if (has_valid_pathfile_rom)
	{
		strncpy(fullpath, pathfile_rom, sizeof(fullpath) - 1);
		fullpath[sizeof(fullpath) - 1] = '\0';
	}
	else if (!SelectRomFromSdmcBrowser(fullpath, sizeof(fullpath)))
	{
		return 0;
	}

	Initialize();
	SetBottomBacklightEnabled(false);
	System_Open(fullpath);
	CPU_Run();
	System_Close();
	SetBottomBacklightEnabled(true);
	
	System_Finalize();
	pglExit();

	return 0;
}
