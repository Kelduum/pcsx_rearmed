/*
 * (C) notaz, 2010-2011
 *
 * This work is licensed under the terms of the GNU GPLv2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "main.h"
#include "plugin.h"
#include "plugin_lib.h"
#include "pcnt.h"
#include "menu.h"
#include "plat.h"
#include "../libpcsxcore/misc.h"
#include "../libpcsxcore/cheat.h"
#include "../libpcsxcore/new_dynarec/new_dynarec.h"
#include "../plugins/cdrcimg/cdrcimg.h"
#include "revision.h"

#ifndef NO_FRONTEND
#include "libpicofe/input.h"
#include "libpicofe/plat.h"
#include "libpicofe/readpng.h"
#endif

// don't include debug.h - it breaks ARM build (R1 redefined)
void StartDebugger();
void StopDebugger();

// sound plugin
extern int iUseReverb;
extern int iUseInterpolation;
extern int iXAPitch;
extern int iVolume;

int ready_to_go;
unsigned long gpuDisp;
char cfgfile_basename[MAXPATHLEN];
int state_slot;
enum sched_action emu_action, emu_action_old;
char hud_msg[64];
int hud_new_msg;

static void make_path(char *buf, size_t size, const char *dir, const char *fname)
{
	if (fname)
		snprintf(buf, size, ".%s%s", dir, fname);
	else
		snprintf(buf, size, ".%s", dir);
}
#define MAKE_PATH(buf, dir, fname) \
	make_path(buf, sizeof(buf), dir, fname)

static void create_profile_dir(const char *directory) {
	char path[MAXPATHLEN];

	MAKE_PATH(path, directory, NULL);
	mkdir(path, S_IRWXU | S_IRWXG);
}

static void CheckSubDir() {
	// make sure that ~/.pcsx exists
	create_profile_dir(PCSX_DOT_DIR);

	create_profile_dir(BIOS_DIR);
	create_profile_dir(MEMCARD_DIR);
	create_profile_dir(STATES_DIR);
	create_profile_dir(PLUGINS_DIR);
	create_profile_dir(PLUGINS_CFG_DIR);
	create_profile_dir(CHEATS_DIR);
	create_profile_dir(PATCHES_DIR);
	create_profile_dir(PCSX_DOT_DIR "cfg");
	create_profile_dir("/screenshots/");
}

static int get_gameid_filename(char *buf, int size, const char *fmt, int i) {
	char trimlabel[33];
	int j;

	strncpy(trimlabel, CdromLabel, 32);
	trimlabel[32] = 0;
	for (j = 31; j >= 0; j--)
		if (trimlabel[j] == ' ')
			trimlabel[j] = 0;
		else
			continue;

	snprintf(buf, size, fmt, trimlabel, CdromId, i);

	return 0;
}

void set_cd_image(const char *fname)
{
	const char *ext = NULL;
	
	if (fname != NULL)
		ext = strrchr(fname, '.');

	if (ext && (
	    strcasecmp(ext, ".z") == 0 || strcasecmp(ext, ".bz") == 0 ||
	    strcasecmp(ext, ".znx") == 0 /*|| strcasecmp(ext, ".pbp") == 0*/)) {
		SetIsoFile(NULL);
		cdrcimg_set_fname(fname);
		strcpy(Config.Cdr, "builtin_cdrcimg");
	} else {
		SetIsoFile(fname);
		strcpy(Config.Cdr, "builtin_cdr");
	}
}

static void set_default_paths(void)
{
	MAKE_PATH(Config.Mcd1, MEMCARD_DIR, "card1.mcd");
	MAKE_PATH(Config.Mcd2, MEMCARD_DIR, "card2.mcd");
	strcpy(Config.BiosDir, "bios");

	strcpy(Config.PluginsDir, "plugins");
	strcpy(Config.Gpu, "builtin_gpu");
	strcpy(Config.Spu, "builtin_spu");
	strcpy(Config.Cdr, "builtin_cdr");
	strcpy(Config.Pad1, "builtin_pad");
	strcpy(Config.Pad2, "builtin_pad");
	strcpy(Config.Net, "Disabled");

	snprintf(Config.PatchesDir, sizeof(Config.PatchesDir), "." PATCHES_DIR);
}

void emu_set_default_config(void)
{
	// try to set sane config on which most games work
	Config.Xa = Config.Cdda = Config.Sio =
	Config.SpuIrq = Config.RCntFix = Config.VSyncWA = 0;
	Config.CdrReschedule = 0;
	Config.PsxAuto = 1;

	pl_rearmed_cbs.gpu_neon.allow_interlace = 2; // auto
	pl_rearmed_cbs.gpu_neon.enhancement_enable =
	pl_rearmed_cbs.gpu_neon.enhancement_no_main = 0;
	pl_rearmed_cbs.gpu_peops.iUseDither = 0;
	pl_rearmed_cbs.gpu_peops.dwActFixes = 1<<7;
	pl_rearmed_cbs.gpu_unai.abe_hack =
	pl_rearmed_cbs.gpu_unai.no_light =
	pl_rearmed_cbs.gpu_unai.no_blend = 0;
	memset(&pl_rearmed_cbs.gpu_peopsgl, 0, sizeof(pl_rearmed_cbs.gpu_peopsgl));
	pl_rearmed_cbs.gpu_peopsgl.iVRamSize = 64;
	pl_rearmed_cbs.gpu_peopsgl.iTexGarbageCollection = 1;

	iUseReverb = 2;
	iUseInterpolation = 1;
	iXAPitch = 0;
	iVolume = 768;
#ifndef __ARM_ARCH_7A__ /* XXX */
	iUseReverb = 0;
	iUseInterpolation = 0;
#endif
	new_dynarec_hacks = 0;
	cycle_multiplier = 200;

	in_type1 = PSE_PAD_TYPE_STANDARD;
	in_type2 = PSE_PAD_TYPE_STANDARD;
}

static void check_memcards(void)
{
	char buf[MAXPATHLEN];
	FILE *f;
	int i;

	for (i = 1; i <= 9; i++) {
		snprintf(buf, sizeof(buf), ".%scard%d.mcd", MEMCARD_DIR, i);

		f = fopen(buf, "rb");
		if (f == NULL) {
			printf("Creating memcard: %s\n", buf);
			CreateMcd(buf);
		}
		else
			fclose(f);
	}
}

void do_emu_action(void)
{
	int ret;

	emu_action_old = emu_action;

	switch (emu_action) {
	case SACTION_LOAD_STATE:
		ret = emu_load_state(state_slot);
		snprintf(hud_msg, sizeof(hud_msg), ret == 0 ? "LOADED" : "FAIL!");
		break;
	case SACTION_SAVE_STATE:
		ret = emu_save_state(state_slot);
		snprintf(hud_msg, sizeof(hud_msg), ret == 0 ? "SAVED" : "FAIL!");
		break;
#ifndef NO_FRONTEND
	case SACTION_ENTER_MENU:
		menu_loop();
		return;
	case SACTION_NEXT_SSLOT:
		state_slot++;
		if (state_slot > 9)
			state_slot = 0;
		goto do_state_slot;
	case SACTION_PREV_SSLOT:
		state_slot--;
		if (state_slot < 0)
			state_slot = 9;
do_state_slot:
		snprintf(hud_msg, sizeof(hud_msg), "STATE SLOT %d [%s]", state_slot,
			emu_check_state(state_slot) == 0 ? "USED" : "FREE");
		hud_new_msg = 3;
		printf("* %s\n", hud_msg);
		break;
	case SACTION_TOGGLE_FSKIP:
		pl_rearmed_cbs.fskip_advice = 0;
		pl_rearmed_cbs.frameskip++;
		if (pl_rearmed_cbs.frameskip > 1)
			pl_rearmed_cbs.frameskip = -1;
		snprintf(hud_msg, sizeof(hud_msg), "FRAMESKIP: %s",
			pl_rearmed_cbs.frameskip == -1 ? "AUTO" :
			pl_rearmed_cbs.frameskip == 0 ? "OFF" : "1" );
		plugin_call_rearmed_cbs();
		break;
	case SACTION_SWITCH_DISPMODE:
		pl_switch_dispmode();
		plugin_call_rearmed_cbs();
		if (GPU_open != NULL && GPU_close != NULL) {
			GPU_close();
			GPU_open(&gpuDisp, "PCSX", NULL);
		}
		break;
	case SACTION_SCREENSHOT:
		{
			char buf[MAXPATHLEN];
			void *scrbuf;
			int w, h, bpp;
			time_t t = time(NULL);
			struct tm *tb = localtime(&t);
			int ti = tb->tm_yday * 1000000 + tb->tm_hour * 10000 +
				tb->tm_min * 100 + tb->tm_sec;

			scrbuf = pl_prepare_screenshot(&w, &h, &bpp);
			get_gameid_filename(buf, sizeof(buf),
				"screenshots/%.32s-%.9s.%d.png", ti);
			ret = -1;
			if (scrbuf != 0 && bpp == 16)
				ret = writepng(buf, scrbuf, w, h);
			if (ret == 0)
				snprintf(hud_msg, sizeof(hud_msg), "SCREENSHOT TAKEN");
			break;
		}
	case SACTION_VOLUME_UP:
	case SACTION_VOLUME_DOWN:
		plat_target_step_volume(emu_action == SACTION_VOLUME_UP);
		return;
	case SACTION_MINIMIZE:
		if (GPU_close != NULL)
			GPU_close();

		plat_minimize();

		if (GPU_open != NULL) {
			ret = GPU_open(&gpuDisp, "PCSX", NULL);
			if (ret)
				fprintf(stderr, "GPU_open returned %d\n", ret);
		}
		return;
#endif
	default:
		return;
	}

	hud_new_msg = 3;
}

static int cdidcmp(const char *id1, const char *id2)
{
	while (*id1 != 0 && *id2 != 0) {
		if (*id1 == '_') { id1++; continue; }
		if (*id2 == '_') { id2++; continue; }
		if (*id1 != *id2)
			break;
		id1++;
		id2++;
	}

	return *id1 - *id2;
}

static void parse_cwcheat(void)
{
	char line[256], buf[64], name[64], *p;
	int newcheat = 1;
	u32 a, v;
	FILE *f;

	f = fopen("cheatpops.db", "r");
	if (f == NULL)
		return;

	/* find the game */
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "_S %63s", buf) != 1)
			continue;
		if (cdidcmp(buf, CdromId) == 0)
			break;
	}

	if (feof(f))
		goto out;

	printf("cwcheat section found for %s\n", CdromId);
	while (fgets(line, sizeof(line), f))
	{
		p = line + strlen(line);
		for (p--; p >= line && (*p == '\r' || *p == '\n' || *p == ' '); p--)
			*p = 0;
		if (*p == 0 || *p == '#' || *p == ';')
			continue;

		if (strncmp(line, "_S", 2) == 0)
			break;
		if (strncmp(line, "_G", 2) == 0) {
			printf("  cwcheat game name: '%s'\n", line + 3);
			continue;
		}
		if (strncmp(line, "_C0", 3) == 0) {
			if (!newcheat && Cheats[NumCheats - 1].n == 0) {
				printf("cheat '%s' failed to parse\n", name);
				free(Cheats[NumCheats - 1].Descr);
				NumCheats--;
			}
			snprintf(name, sizeof(name), "%s", line + 4);
			newcheat = 1;
			continue;
		}
		if (sscanf(line, "_L %x %x", &a, &v) != 2) {
			printf("line failed to parse: '%s'\n", line);
			continue;
		}

		if (newcheat) {
			if (NumCheats >= NumCheatsAllocated) {
				NumCheatsAllocated += 16;
				Cheats = realloc(Cheats, sizeof(Cheat) *
						NumCheatsAllocated);
				if (Cheats == NULL)
					break;
			}
			Cheats[NumCheats].Descr = strdup(name);
			Cheats[NumCheats].Enabled = 0;
			Cheats[NumCheats].WasEnabled = 0;
			Cheats[NumCheats].First = NumCodes;
			Cheats[NumCheats].n = 0;
			NumCheats++;
			newcheat = 0;
		}

		if (NumCodes >= NumCodesAllocated) {
			NumCodesAllocated += 16;
			CheatCodes = realloc(CheatCodes, sizeof(CheatCode) *
				NumCodesAllocated);
			if (CheatCodes == NULL)
				break;
		}
		CheatCodes[NumCodes].Addr = a;
		CheatCodes[NumCodes].Val = v;
		NumCodes++;
		Cheats[NumCheats - 1].n++;
	}

out:
	fclose(f);
}

void emu_on_new_cd(void)
{
	ClearAllCheats();
	parse_cwcheat();

	if (Config.HLE) {
		printf("note: running with HLE BIOS, expect compatibility problems\n");
		printf("----------------------------------------------------------\n");
	}

	snprintf(hud_msg, sizeof(hud_msg), "Booting up...");
	hud_new_msg = 2;
}

int emu_core_preinit(void)
{
	// what is the name of the config file?
	// it may be redefined by -cfg on the command line
	strcpy(cfgfile_basename, "pcsx.cfg");

	emuLog = stdout;
	SetIsoFile(NULL);

	memset(&Config, 0, sizeof(Config));

	set_default_paths();
	emu_set_default_config();
	strcpy(Config.Bios, "HLE");

	return 0;
}

int emu_core_init(void)
{
	CheckSubDir();
	check_memcards();

	if (EmuInit() == -1) {
		printf("PSX emulator couldn't be initialized.\n");
		return -1;
	}

	LoadMcds(Config.Mcd1, Config.Mcd2);

	if (Config.Debug) {
		StartDebugger();
	}

	return 0;
}

#ifndef NO_FRONTEND
int main(int argc, char *argv[])
{
	char file[MAXPATHLEN] = "";
	char path[MAXPATHLEN];
	const char *cdfile = NULL;
	const char *loadst_f = NULL;
	int psxout = 0;
	int loadst = 0;
	int i;

	emu_core_preinit();

	// read command line options
	for (i = 1; i < argc; i++) {
		     if (!strcmp(argv[i], "-psxout")) psxout = 1;
		else if (!strcmp(argv[i], "-load")) loadst = atol(argv[++i]);
		else if (!strcmp(argv[i], "-cfg")) {
			if (i+1 >= argc) break;
			strncpy(cfgfile_basename, argv[++i], MAXPATHLEN-100);	/* TODO buffer overruns */
			printf("Using config file %s.\n", cfgfile_basename);
		}
		else if (!strcmp(argv[i], "-cdfile")) {
			char isofilename[MAXPATHLEN];

			if (i+1 >= argc) break;
			strncpy(isofilename, argv[++i], MAXPATHLEN);
			if (isofilename[0] != '/') {
				getcwd(path, MAXPATHLEN);
				if (strlen(path) + strlen(isofilename) + 1 < MAXPATHLEN) {
					strcat(path, "/");
					strcat(path, isofilename);
					strcpy(isofilename, path);
				} else
					isofilename[0] = 0;
			}

			cdfile = isofilename;
		}
		else if (!strcmp(argv[i], "-loadf")) {
			if (i+1 >= argc) break;
			loadst_f = argv[++i];
		}
		else if (!strcmp(argv[i], "-h") ||
			 !strcmp(argv[i], "-help") ||
			 !strcmp(argv[i], "--help")) {
			 printf("PCSX-ReARMed " REV "\n");
			 printf("%s\n", _(
							" pcsx [options] [file]\n"
							"\toptions:\n"
							"\t-cdfile FILE\tRuns a CD image file\n"
							"\t-cfg FILE\tLoads desired configuration file (default: ~/.pcsx/pcsx.cfg)\n"
							"\t-psxout\t\tEnable PSX output\n"
							"\t-load STATENUM\tLoads savestate STATENUM (1-5)\n"
							"\t-h -help\tDisplay this message\n"
							"\tfile\t\tLoads a PSX EXE file\n"));
			 return 0;
		} else {
			strncpy(file, argv[i], MAXPATHLEN);
			if (file[0] != '/') {
				getcwd(path, MAXPATHLEN);
				if (strlen(path) + strlen(file) + 1 < MAXPATHLEN) {
					strcat(path, "/");
					strcat(path, file);
					strcpy(file, path);
				} else
					file[0] = 0;
			}
		}
	}

	if (cdfile)
		set_cd_image(cdfile);

	// frontend stuff
	// init input but leave probing to platform code,
	// they add input drivers and may need to modify them after probe
	in_init();
	pl_init();
	plat_init();
	menu_init(); // loads config

	emu_core_init();

	if (psxout)
		Config.PsxOut = 1;

	if (LoadPlugins() == -1) {
		// FIXME: this recovery doesn't work, just delete bad config and bail out
		// SysMessage("could not load plugins, retrying with defaults\n");
		set_default_paths();
		snprintf(path, sizeof(path), "." PCSX_DOT_DIR "%s", cfgfile_basename);
		remove(path);
		SysMessage("Failed loading plugins!");
		return 1;
	}
	pcnt_hook_plugins();

	if (OpenPlugins() == -1) {
		return 1;
	}
	plugin_call_rearmed_cbs();

	CheckCdrom();
	SysReset();

	if (file[0] != '\0') {
		if (Load(file) != -1)
			ready_to_go = 1;
	} else {
		if (cdfile) {
			if (LoadCdrom() == -1) {
				ClosePlugins();
				printf(_("Could not load CD-ROM!\n"));
				return -1;
			}
			emu_on_new_cd();
			ready_to_go = 1;
		}
	}

	if (ready_to_go) {
		menu_prepare_emu();

		// If a state has been specified, then load that
		if (loadst) {
			int ret = emu_load_state(loadst - 1);
			printf("%s state %d\n", ret ? "failed to load" : "loaded", loadst);
		}
		if (loadst_f) {
			int ret = LoadState(loadst_f);
			printf("%s state file: %s\n", ret ? "failed to load" : "loaded", loadst_f);
		}
	}
	else
		menu_loop();

	pl_start_watchdog();

	while (1)
	{
		stop = 0;
		emu_action = SACTION_NONE;

		psxCpu->Execute();
		if (emu_action != SACTION_NONE)
			do_emu_action();
	}

	return 0;
}
#endif

void SysRunGui() {
        printf("SysRunGui\n");
}

static void dummy_lace()
{
}

void SysReset() {
	// rearmed hack: EmuReset() runs some code when real BIOS is used,
	// but we usually do reset from menu while GPU is not open yet,
	// so we need to prevent updateLace() call..
	void *real_lace = GPU_updateLace;
	GPU_updateLace = dummy_lace;

	// reset can run code, timing must be set
	pl_timing_prepare(Config.PsxType);

	EmuReset();

	// hmh core forgets this
	CDR_stop();

	GPU_updateLace = real_lace;
}

void SysClose() {
	EmuShutdown();
	ReleasePlugins();

	StopDebugger();

	if (emuLog != NULL) fclose(emuLog);
}

void SysUpdate() {
}

void OnFile_Exit() {
	printf("OnFile_Exit\n");
	SysClose();
#ifndef NO_FRONTEND
	menu_finish();
	plat_finish();
	exit(0);
#endif
}

int get_state_filename(char *buf, int size, int i) {
	return get_gameid_filename(buf, size,
		"." STATES_DIR "%.32s-%.9s.%3.3d", i);
}

int emu_check_state(int slot)
{
	char fname[MAXPATHLEN];
	int ret;

	ret = get_state_filename(fname, sizeof(fname), slot);
	if (ret != 0)
		return ret;

	return CheckState(fname);
}

int emu_save_state(int slot)
{
	char fname[MAXPATHLEN];
	int ret;

	ret = get_state_filename(fname, sizeof(fname), slot);
	if (ret != 0)
		return ret;

	ret = SaveState(fname);
#ifndef __ARM_ARCH_7A__ /* XXX */
	sync();
#endif
	printf("* %s \"%s\" [%d]\n", ret == 0 ? "saved" : "failed to save", fname, slot);
	return ret;
}

int emu_load_state(int slot)
{
	char fname[MAXPATHLEN];
	int ret;

	ret = get_state_filename(fname, sizeof(fname), slot);
	if (ret != 0)
		return ret;

	return LoadState(fname);
}

void SysPrintf(const char *fmt, ...) {
	va_list list;
	char msg[512];

	va_start(list, fmt);
	vsprintf(msg, fmt, list);
	va_end(list);

	fprintf(emuLog, "%s", msg);
}

void SysMessage(const char *fmt, ...) {
        va_list list;
        char msg[512];

        va_start(list, fmt);
        vsprintf(msg, fmt, list);
        va_end(list);

        if (msg[strlen(msg) - 1] == '\n')
                msg[strlen(msg) - 1] = 0;

	fprintf(stderr, "%s\n", msg);
}

static void SignalExit(int sig) {
	ClosePlugins();
	OnFile_Exit();
}

#define PARSEPATH(dst, src) \
	ptr = src + strlen(src); \
	while (*ptr != '\\' && ptr != src) ptr--; \
	if (ptr != src) { \
		strcpy(dst, ptr+1); \
	}

static int _OpenPlugins(void) {
	int ret;

	signal(SIGINT, SignalExit);
	signal(SIGPIPE, SignalExit);

	GPU_clearDynarec(clearDynarec);

	ret = CDR_open();
	if (ret < 0) { SysMessage(_("Error opening CD-ROM plugin!")); return -1; }
	ret = SPU_open();
	if (ret < 0) { SysMessage(_("Error opening SPU plugin!")); return -1; }
	SPU_registerCallback(SPUirq);
	// pcsx-rearmed: we handle gpu elsewhere
	//ret = GPU_open(&gpuDisp, "PCSX", NULL);
	//if (ret < 0) { SysMessage(_("Error opening GPU plugin!")); return -1; }
	ret = PAD1_open(&gpuDisp);
	if (ret < 0) { SysMessage(_("Error opening Controller 1 plugin!")); return -1; }
	ret = PAD2_open(&gpuDisp);
	if (ret < 0) { SysMessage(_("Error opening Controller 2 plugin!")); return -1; }

	if (Config.UseNet && !NetOpened) {
		netInfo info;
		char path[MAXPATHLEN];
		char dotdir[MAXPATHLEN];

		MAKE_PATH(dotdir, "/.pcsx/plugins/", NULL);

		strcpy(info.EmuName, "PCSX");
		strncpy(info.CdromID, CdromId, 9);
		strncpy(info.CdromLabel, CdromLabel, 9);
		info.psxMem = psxM;
		info.GPU_showScreenPic = GPU_showScreenPic;
		info.GPU_displayText = GPU_displayText;
		info.GPU_showScreenPic = GPU_showScreenPic;
		info.PAD_setSensitive = PAD1_setSensitive;
		sprintf(path, "%s%s", Config.BiosDir, Config.Bios);
		strcpy(info.BIOSpath, path);
		strcpy(info.MCD1path, Config.Mcd1);
		strcpy(info.MCD2path, Config.Mcd2);
		sprintf(path, "%s%s", dotdir, Config.Gpu);
		strcpy(info.GPUpath, path);
		sprintf(path, "%s%s", dotdir, Config.Spu);
		strcpy(info.SPUpath, path);
		sprintf(path, "%s%s", dotdir, Config.Cdr);
		strcpy(info.CDRpath, path);
		NET_setInfo(&info);

		ret = NET_open(&gpuDisp);
		if (ret < 0) {
			if (ret == -2) {
				// -2 is returned when something in the info
				// changed and needs to be synced
				char *ptr;

				PARSEPATH(Config.Bios, info.BIOSpath);
				PARSEPATH(Config.Gpu,  info.GPUpath);
				PARSEPATH(Config.Spu,  info.SPUpath);
				PARSEPATH(Config.Cdr,  info.CDRpath);

				strcpy(Config.Mcd1, info.MCD1path);
				strcpy(Config.Mcd2, info.MCD2path);
				return -2;
			} else {
				Config.UseNet = FALSE;
			}
		} else {
			if (NET_queryPlayer() == 1) {
				if (SendPcsxInfo() == -1) Config.UseNet = FALSE;
			} else {
				if (RecvPcsxInfo() == -1) Config.UseNet = FALSE;
			}
		}
		NetOpened = TRUE;
	} else if (Config.UseNet) {
		NET_resume();
	}

	return 0;
}

int OpenPlugins() {
	int ret;

	while ((ret = _OpenPlugins()) == -2) {
		ReleasePlugins();
		LoadMcds(Config.Mcd1, Config.Mcd2);
		if (LoadPlugins() == -1) return -1;
	}
	return ret;
}

void ClosePlugins() {
	int ret;

	signal(SIGINT, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	ret = CDR_close();
	if (ret < 0) { SysMessage(_("Error closing CD-ROM plugin!")); return; }
	ret = SPU_close();
	if (ret < 0) { SysMessage(_("Error closing SPU plugin!")); return; }
	ret = PAD1_close();
	if (ret < 0) { SysMessage(_("Error closing Controller 1 Plugin!")); return; }
	ret = PAD2_close();
	if (ret < 0) { SysMessage(_("Error closing Controller 2 plugin!")); return; }
	// pcsx-rearmed: we handle gpu elsewhere
	//ret = GPU_close();
	//if (ret < 0) { SysMessage(_("Error closing GPU plugin!")); return; }

	if (Config.UseNet) {
		NET_pause();
	}
}

/* we hook statically linked plugins here */
static const char *builtin_plugins[] = {
	"builtin_gpu", "builtin_spu", "builtin_cdr", "builtin_pad",
	"builtin_cdrcimg",
};

static const int builtin_plugin_ids[] = {
	PLUGIN_GPU, PLUGIN_SPU, PLUGIN_CDR, PLUGIN_PAD,
	PLUGIN_CDRCIMG,
};

void *SysLoadLibrary(const char *lib) {
	const char *tmp = strrchr(lib, '/');
	void *ret;
	int i;

	printf("plugin: %s\n", lib);

	if (tmp != NULL) {
		tmp++;
		for (i = 0; i < ARRAY_SIZE(builtin_plugins); i++)
			if (strcmp(tmp, builtin_plugins[i]) == 0)
				return (void *)(long)(PLUGIN_DL_BASE + builtin_plugin_ids[i]);
	}

#if defined(__x86_64__) || defined(__i386__)
	// convenience hack
	if (strstr(lib, ".x86") == NULL) {
		char name[MAXPATHLEN];
		snprintf(name, sizeof(name), "%s.x86_64", lib);
		lib = name;
	}
#endif

	ret = dlopen(lib, RTLD_NOW);
	if (ret == NULL)
		fprintf(stderr, "dlopen: %s\n", dlerror());
	return ret;
}

void *SysLoadSym(void *lib, const char *sym) {
	unsigned int plugid = (unsigned int)(long)lib;

	if (PLUGIN_DL_BASE <= plugid && plugid < PLUGIN_DL_BASE + ARRAY_SIZE(builtin_plugins))
		return plugin_link(plugid - PLUGIN_DL_BASE, sym);

	return dlsym(lib, sym);
}

const char *SysLibError() {
	return dlerror();
}

void SysCloseLibrary(void *lib) {
	unsigned int plugid = (unsigned int)(long)lib;

	if (PLUGIN_DL_BASE <= plugid && plugid < PLUGIN_DL_BASE + ARRAY_SIZE(builtin_plugins))
		return;

	dlclose(lib);
}

