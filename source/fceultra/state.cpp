/* FCE Ultra - NES/Famicom Emulator
*
* Copyright notice for this file:
*  Copyright (C) 2002 Xodnizel
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

//  TODO: Add (better) file io error checking

#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <unistd.h> //mbg merge 7/17/06 removed

#include <vector>
#include <fstream>

#include "version.h"
#include "types.h"
#include "x6502.h"
#include "fceu.h"
#include "sound.h"
#include "utils/endian.h"
#include "utils/memory.h"
#include "utils/memorystream.h"
#include "utils/xstring.h"
#include "file.h"
#include "fds.h"
#include "state.h"
#include "movie.h"
#include "ppu.h"
#include "netplay.h"
#include "video.h"
#include "input.h"
#include "zlib.h"
#include "driver.h"
#ifdef _S9XLUA_H
#include "fceulua.h"
#endif

//TODO - we really need some kind of global platform-specific options api
#ifdef WIN32
#include "drivers/win/main.h"
#include "drivers/win/ram_search.h"
#include "drivers/win/ramwatch.h"
#endif

using namespace std;

static void (*SPreSave)(void);
static void (*SPostSave)(void);

static int SaveStateStatus[10];
static int StateShow;

//tells the save system innards that we're loading the old format
bool FCEU_state_loading_old_format;

char lastSavestateMade[2048]; //Stores the filename of the last savestate made (needed for UndoSavestate)
bool undoSS = false;		  //This will be true if there is lastSavestateMade, it was made since ROM was loaded, a backup state for lastSavestateMade exists
bool redoSS = false;		  //This will be true if UndoSaveState is run, will turn false when a new savestate is made

char lastLoadstateMade[2048]; //Stores the filename of the last state loaded (needed for Undo/Redo loadstate)
bool undoLS = false;		  //This will be true if a backupstate was made and it was made since ROM was loaded
bool redoLS = false;		  //This will be true if a backupstate was loaded, meaning redoLoadState can be run

bool internalSaveLoad = false;

#define SFMDATA_SIZE (64)
static SFORMAT SFMDATA[SFMDATA_SIZE];
static int SFEXINDEX;

#define RLSB 		FCEUSTATE_RLSB	//0x80000000


extern SFORMAT FCEUPPU_STATEINFO[];
extern SFORMAT FCEU_NEWPPU_STATEINFO[];
extern SFORMAT FCEUSND_STATEINFO[];
extern SFORMAT FCEUCTRL_STATEINFO[];
extern SFORMAT FCEUMOV_STATEINFO[];


SFORMAT SFCPU[]={
	{ &X.PC, 2|RLSB, "PC\0"},
	{ &X.A, 1, "A\0\0"},
	{ &X.P, 1, "P\0\0"},
	{ &X.X, 1, "X\0\0"},
	{ &X.Y, 1, "Y\0\0"},
	{ &X.S, 1, "S\0\0"},
	{ &RAM, 0x800 | FCEUSTATE_INDIRECT, "RAM", },
	{ 0 }
};

SFORMAT SFCPUC[]={
	{ &X.jammed, 1, "JAMM"},
	{ &X.IRQlow, 4|RLSB, "IQLB"},
	{ &X.tcount, 4|RLSB, "ICoa"},
	{ &X.count,  4|RLSB, "ICou"},
	{ &timestampbase, sizeof(timestampbase) | RLSB, "TSBS"},
	{ &X.mooPI, 1, "MooP"}, // alternative to the "quick and dirty hack"
	{ 0 }
};

void foo(uint8* test) { (void)test; }

static int SubWrite(std::ostream* os, SFORMAT *sf)
{
	uint32 acc=0;

	while(sf->v)
	{
		if(sf->s==~0)		//Link to another struct
		{
			uint32 tmp;

			if(!(tmp=SubWrite(os,(SFORMAT *)sf->v)))
				return(0);
			acc+=tmp;
			sf++;
			continue;
		}

		acc+=8;			//Description + size
		acc+=sf->s&(~FCEUSTATE_FLAGS);

		if(os)			//Are we writing or calculating the size of this block?
		{
			os->write(sf->desc,4);
			write32le(sf->s&(~FCEUSTATE_FLAGS),os);

#ifndef LSB_FIRST
			if(sf->s&RLSB)
				FlipByteOrder((uint8*)sf->v,sf->s&(~FCEUSTATE_FLAGS));
#endif

			if(sf->s&FCEUSTATE_INDIRECT)
				os->write(*(char **)sf->v,sf->s&(~FCEUSTATE_FLAGS));
			else
				os->write((char*)sf->v,sf->s&(~FCEUSTATE_FLAGS));

			//Now restore the original byte order.
#ifndef LSB_FIRST
			if(sf->s&RLSB)
				FlipByteOrder((uint8*)sf->v,sf->s&(~FCEUSTATE_FLAGS));
#endif
		}
		sf++;
	}

	return(acc);
}

static int WriteStateChunk(std::ostream* os, int type, SFORMAT *sf)
{
	os->put(type);
	int bsize = SubWrite((std::ostream*)0,sf);
	write32le(bsize,os);

	if(!SubWrite(os,sf))
	{
		return 5;
	}
	return (bsize+5);
}

static SFORMAT *CheckS(SFORMAT *sf, uint32 tsize, char *desc)
{
	while(sf->v)
	{
		if(sf->s==~0)		// Link to another SFORMAT structure.
		{
			SFORMAT *tmp;
			if((tmp= CheckS((SFORMAT *)sf->v, tsize, desc) ))
				return(tmp);
			sf++;
			continue;
		}
		if(!memcmp(desc,sf->desc,4))
		{
			if(tsize!=(sf->s&(~FCEUSTATE_FLAGS)))
				return(0);
			return(sf);
		}
		sf++;
	}
	return(0);
}

static bool ReadStateChunk(std::istream* is, SFORMAT *sf, int size)
{
	SFORMAT *tmp;
	int temp = is->tellg();

	while(is->tellg()<temp+size)
	{
		uint32 tsize;
		char toa[4];
		if(is->read(toa,4).gcount()<4)
			return false;

		read32le(&tsize,is);

		if((tmp=CheckS(sf,tsize,toa)))
		{
			if(tmp->s&FCEUSTATE_INDIRECT)
				is->read(*(char **)tmp->v,tmp->s&(~FCEUSTATE_FLAGS));
			else
				is->read((char *)tmp->v,tmp->s&(~FCEUSTATE_FLAGS));

#ifndef LSB_FIRST
			if(tmp->s&RLSB)
				FlipByteOrder((uint8*)tmp->v,tmp->s&(~FCEUSTATE_FLAGS));
#endif
		}
		else
			is->seekg(tsize,std::ios::cur);
	} // while(...)
	return true;
}

static int read_sfcpuc=0, read_snd=0;

void FCEUD_BlitScreen(uint8 *XBuf); //mbg merge 7/17/06 YUCKY had to add
void UpdateFCEUWindow(void);  //mbg merge 7/17/06 YUCKY had to add
static bool ReadStateChunks(std::istream* is, int32 totalsize)
{
	int t;
	uint32 size;
	bool ret=true;
	bool warned=false;

	read_sfcpuc=0;
	read_snd=0;

	//mbg 6/16/08 - wtf
	//// int moo=X.mooPI;
	// if(!scan_chunks)
	//   X.mooPI=/*X.P*/0xFF;

	while(totalsize > 0)
	{
		t=is->get();
		if(t==EOF) break;
		if(!read32le(&size,is)) break;
		totalsize -= size + 5;

		switch(t)
		{
		case 1:if(!ReadStateChunk(is,SFCPU,size)) ret=false;break;
		case 3:if(!ReadStateChunk(is,FCEUPPU_STATEINFO,size)) ret=false;break;
		case 31:if(!ReadStateChunk(is,FCEU_NEWPPU_STATEINFO,size)) ret=false;break;
		case 4:if(!ReadStateChunk(is,FCEUCTRL_STATEINFO,size)) ret=false;break;
		case 7:
			if(!FCEUMOV_ReadState(is,size)) {
				//allow this to fail in old-format savestates.
				if(!FCEU_state_loading_old_format)
					ret=false;
			}
			break;
		case 0x10:if(!ReadStateChunk(is,SFMDATA,size)) ret=false; break;

			// now it gets hackier:
		case 5:
			if(!ReadStateChunk(is,FCEUSND_STATEINFO,size))
				ret=false;
			else
				read_snd=1;
			break;
		case 6:
			if(FCEUMOV_Mode(MOVIEMODE_PLAY|MOVIEMODE_RECORD))
			{
				if(!ReadStateChunk(is,FCEUMOV_STATEINFO,size)) ret=false;
			}
			else
			{
				is->seekg(size,std::ios::cur);
			}
			break;
		case 8:
			// load back buffer
			{
				extern uint8 *XBackBuf;
				if(is->read((char*)XBackBuf,size).gcount() != size)
					ret = false;

				//MBG TODO - can this be moved to a better place?
				//does it even make sense, displaying XBuf when its XBackBuf we just loaded?
#ifdef WIN32
				else
				{
					FCEUD_BlitScreen(XBuf);
					UpdateFCEUWindow();
				}
#endif

			}
			break;
		case 2:
			{
				if(!ReadStateChunk(is,SFCPUC,size))
					ret=false;
				else
					read_sfcpuc=1;
			}  break;
		default:
			// for somebody's sanity's sake, at least warn about it:
			if(!warned)
			{
				char str [256];
				sprintf(str, "Warning: Found unknown save chunk of type %d.\nThis could indicate the save state is corrupted\nor made with a different (incompatible) emulator version.", t);
				FCEUD_PrintError(str);
				warned=true;
			}
			//if(fseek(st,size,SEEK_CUR)<0) goto endo;break;
			is->seekg(size,std::ios::cur);
		}
	}
	//endo:

	//mbg 6/16/08 - wtf
	// if(X.mooPI==0xFF && !scan_chunks)
	// {
	////	 FCEU_PrintError("prevmoo=%d, p=%d",moo,X.P);
	//   X.mooPI=X.P; // "Quick and dirty hack." //begone
	// }

	extern int resetDMCacc;
	if(read_snd)
		resetDMCacc=0;
	else
		resetDMCacc=1;

	return ret;
}

int CurrentState=0;
extern int geniestage;


bool FCEUSS_SaveMS(std::ostream* outstream, int compressionLevel)
{
	//a temp memory stream. we'll dump some data here and then compress
	//TODO - support dumping directly without compressing to save a buffer copy

#ifdef GEKKO
	memorystream ms(512*1024); // set aside some space, otherwise expand fails on Wii!
#else
	memorystream ms;
#endif
	std::ostream* os = (std::ostream*)&ms;

	uint32 totalsize = 0;

	FCEUPPU_SaveState();
	FCEUSND_SaveState();
	totalsize=WriteStateChunk(os,1,SFCPU);
	totalsize+=WriteStateChunk(os,2,SFCPUC);
	totalsize+=WriteStateChunk(os,3,FCEUPPU_STATEINFO);
	totalsize+=WriteStateChunk(os,31,FCEU_NEWPPU_STATEINFO);
	totalsize+=WriteStateChunk(os,4,FCEUCTRL_STATEINFO);
	totalsize+=WriteStateChunk(os,5,FCEUSND_STATEINFO);
	if(FCEUMOV_Mode(MOVIEMODE_PLAY|MOVIEMODE_RECORD))
	{
		totalsize+=WriteStateChunk(os,6,FCEUMOV_STATEINFO);

		//MBG tasedit HACK HACK HACK!
		//do not save the movie state if we are in tasedit! that is a huge waste of time and space!
		if(!FCEUMOV_Mode(MOVIEMODE_TASEDIT))
		{
			os->seekp(5,std::ios::cur);
			int size = FCEUMOV_WriteState(os);
			os->seekp(-(size+5),std::ios::cur);
			os->put(7);
			write32le(size, os);
			os->seekp(size,std::ios::cur);

			totalsize += 5 + size;
		}
	}
	// save back buffer
	{
		extern uint8 *XBackBuf;
		uint32 size = 256 * 256 + 8;
		os->put(8);
		write32le(size, os);
		os->write((char*)XBackBuf,size);
		totalsize += 5 + size;
	}

	if(SPreSave) SPreSave();
	totalsize+=WriteStateChunk(os,0x10,SFMDATA);
	if(SPreSave) SPostSave();

	//save the length of the file
#ifdef GEKKO
	int len = ms.tellp();
#else
	int len = ms.size();
#endif

	//sanity check: len and totalsize should be the same
	if(len != totalsize)
	{
		FCEUD_PrintError("sanity violation: len != totalsize");
		return false;
	}

	int error = Z_OK;
	uint8* cbuf = (uint8*)ms.buf();
	uLongf comprlen = -1;
	if(compressionLevel != Z_NO_COMPRESSION)
	{
		//worst case compression.
		//zlib says "0.1% larger than sourceLen plus 12 bytes"
		comprlen = (len>>9)+12 + len;
		cbuf = new uint8[comprlen];
		error = compress2(cbuf,&comprlen,(uint8*)ms.buf(),len,compressionLevel);
	}

	//dump the header
	uint8 header[16]="FCSX";
	FCEU_en32lsb(header+4, totalsize);
	FCEU_en32lsb(header+8, FCEU_VERSION_NUMERIC);
	FCEU_en32lsb(header+12, comprlen);

	//dump it to the destination file
	outstream->write((char*)header,16);
	outstream->write((char*)cbuf,comprlen==-1?totalsize:comprlen);

	if(cbuf != (uint8*)ms.buf()) delete[] cbuf;
	return error == Z_OK;
}


void FCEUSS_Save(const char *fname)
{
	std::fstream* st = 0;
	char fn[2048];

	if(geniestage==1)
	{
		FCEU_DispMessage("Cannot save FCS in GG screen.");
		return;
	}

	if(fname)	//If filename is given use it.
	{
		st =FCEUD_UTF8_fstream(fname, "wb");
		strcpy(fn, fname);
	}
	else		//Else, generate one
	{
		//FCEU_PrintError("daCurrentState=%d",CurrentState);
		strcpy(fn, FCEU_MakeFName(FCEUMKF_STATE,CurrentState,0).c_str());

		//backup existing savestate first
		if (CheckFileExists(fn)) 
		{
			CreateBackupSaveState(fn);		//Make a backup of previous savestate before overwriting it
			strcpy(lastSavestateMade,fn);	//Remember what the last savestate filename was (for undoing later)
			undoSS = true;					//Backup was created so undo is possible
		}
		else
			undoSS = false;					//so backup made so lastSavestateMade does have a backup file, so no undo
		
		st = FCEUD_UTF8_fstream(fn,"wb");
	}

	if(st == NULL)
	{
		FCEU_DispMessage("State %d save error.",CurrentState);
		return;
	}

	#ifdef _S9XLUA_H
	if (!internalSaveLoad)
	{
		LuaSaveData saveData;
		CallRegisteredLuaSaveFunctions(CurrentState, saveData);

		char luaSaveFilename [512];
		strncpy(luaSaveFilename, fn, 512);
		luaSaveFilename[512-(1+7/*strlen(".luasav")*/)] = '\0';
		strcat(luaSaveFilename, ".luasav");
		if(saveData.recordList)
		{
			FILE* luaSaveFile = fopen(luaSaveFilename, "wb");
			if(luaSaveFile)
			{
				saveData.ExportRecords(luaSaveFile);
				fclose(luaSaveFile);
			}
		}
		else
		{
			unlink(luaSaveFilename);
		}
	}
	#endif

	if(FCEUMOV_Mode(MOVIEMODE_INACTIVE))
		FCEUSS_SaveMS(st,-1);
	else
		FCEUSS_SaveMS(st,0);

	delete st;

	if(!fname)
	{
		SaveStateStatus[CurrentState]=1;
		FCEU_DispMessage("State %d saved.",CurrentState);
	}
		redoSS = false;					//we have a new savestate so redo is not possible
}

int FCEUSS_LoadFP_old(std::istream* is, ENUM_SSLOADPARAMS params)
{
	//if(params==SSLOADPARAM_DUMMY && suppress_scan_chunks)
	//	return 1;

	int x;
	uint8 header[16];
	int stateversion;
	char* fn=0;

	////Make temporary savestate in case something screws up during the load
	//if(params == SSLOADPARAM_BACKUP)
	//{
	//	fn=FCEU_MakeFName(FCEUMKF_NPTEMP,0,0);
	//	FILE *fp;
	//	
	//	if((fp=fopen(fn,"wb")))
	//	{
	//		if(FCEUSS_SaveFP(fp))
	//		{
	//			fclose(fp);
	//		}
	//		else
	//		{
	//			fclose(fp);
	//			unlink(fn);
	//			free(fn);
	//			fn=0;
	//		}
	//	}
	//}

	//if(params!=SSLOADPARAM_DUMMY)
	{
		FCEUMOV_PreLoad();
	}
	is->read((char*)&header,16);
	if(memcmp(header,"FCS",3))
	{
		return(0);
	}
	if(header[3] == 0xFF)
	{
		stateversion = FCEU_de32lsb(header + 8);
	}
	else
	{
		stateversion=header[3] * 100;
	}
	//if(params == SSLOADPARAM_DUMMY)
	//{
	//	scan_chunks=1;
	//}
	x=ReadStateChunks(is,*(uint32*)(header+4));
	//if(params == SSLOADPARAM_DUMMY)
	//{
	//	scan_chunks=0;
	//	return 1;
	//}
	if(read_sfcpuc && stateversion<9500)
	{
		X.IRQlow=0;
	}
	if(GameStateRestore)
	{
		GameStateRestore(stateversion);
	}
	if(x)
	{
		FCEUPPU_LoadState(stateversion);
		FCEUSND_LoadState(stateversion);  
		x=FCEUMOV_PostLoad();
	}
	if(fn)
	{
		//if(!x || params == SSLOADPARAM_DUMMY)  //is make_backup==2 possible??  oh well.
		//{
		//	* Oops!  Load the temporary savestate */
		//	FILE *fp;
		//		
		//	if((fp=fopen(fn,"rb")))
		//	{
		//		FCEUSS_LoadFP(fp,SSLOADPARAM_NOBACKUP);
		//		fclose(fp);
		//	}
		//	unlink(fn);
		//}
		free(fn);
	}

	return(x);
}


bool FCEUSS_LoadFP(std::istream* is, ENUM_SSLOADPARAMS params)
{
	//maybe make a backup savestate
	memorystream msBackupSavestate;
	bool backup = (params == SSLOADPARAM_BACKUP);
	if(backup)
		FCEUSS_SaveMS(&msBackupSavestate,Z_NO_COMPRESSION);

	uint8 header[16];

	//read and analyze the header
	is->read((char*)&header,16);
	if(memcmp(header,"FCSX",4)) {
		//its not an fceux save file.. perhaps it is an fceu savefile
		is->seekg(0);
		FCEU_state_loading_old_format = true;
		bool ret = FCEUSS_LoadFP_old(is,params)!=0;
		FCEU_state_loading_old_format = false;
		if(!ret && backup) FCEUSS_LoadFP(&msBackupSavestate,SSLOADPARAM_NOBACKUP);
		return ret;
	}
		
	int totalsize = FCEU_de32lsb(header + 4);
	int stateversion = FCEU_de32lsb(header + 8);
	int comprlen = FCEU_de32lsb(header + 12);

	std::vector<char> buf(totalsize);

	//not compressed:
	if(comprlen != -1)
	{
		//load the compressed chunk and decompress
		std::vector<char> cbuf(comprlen);
		is->read((char*)&cbuf[0],comprlen);

		uLongf uncomprlen = totalsize;
		int error = uncompress((uint8*)&buf[0],&uncomprlen,(uint8*)&cbuf[0],comprlen);
		if(error != Z_OK || uncomprlen != totalsize)
			return false;
		//we dont need to restore the backup here because we havent messed with the emulator state yet
	}
	else
	{
		is->read((char*)&buf[0],totalsize);
	}

	FCEUMOV_PreLoad();

	memorystream mstemp(&buf);
	bool x = ReadStateChunks(&mstemp,totalsize)!=0;

	//mbg 5/24/08 - we don't support old states, so this shouldnt matter.
	//if(read_sfcpuc && stateversion<9500)
	//	X.IRQlow=0;

	if(GameStateRestore)
	{
		GameStateRestore(stateversion);
	}
	if(x)
	{
		FCEUPPU_LoadState(stateversion);
		FCEUSND_LoadState(stateversion);
		x=FCEUMOV_PostLoad();
	}

	if(!x && backup) {
		msBackupSavestate.sync();
		FCEUSS_LoadFP(&msBackupSavestate,SSLOADPARAM_NOBACKUP);
	}

	return x;
}


bool FCEUSS_Load(const char *fname)
{
	std::fstream* st;
	char fn[2048];

	//mbg movie - this needs to be overhauled
	////this fixes read-only toggle problems
	//if(FCEUMOV_IsRecording()) {
	//	FCEUMOV_AddCommand(0);
	//	MovieFlushHeader();
	//}

	if(geniestage==1)
	{
		FCEU_DispMessage("Cannot load FCS in GG screen.");
		return false;
	}
	if(fname)
	{
		st=FCEUD_UTF8_fstream(fname, "rb");
		strcpy(fn, fname);
	}
	else
	{
		strcpy(fn, FCEU_MakeFName(FCEUMKF_STATE,CurrentState,fname).c_str());
		st=FCEUD_UTF8_fstream(fn,"rb");
		strcpy(lastLoadstateMade,fn);
	}

	if(st == NULL)
	{
		FCEU_DispMessage("State %d load error.",CurrentState);
		SaveStateStatus[CurrentState]=0;
		return false;
	}

	//If in bot mode, don't do a backup when loading.
	//Otherwise you eat at the hard disk, since so many
	//states are being loaded.
	if(FCEUSS_LoadFP(st,SSLOADPARAM_BACKUP))
	{
		if(fname)
		{
			char szFilename[260]={0};
			splitpath(fname, 0, 0, szFilename, 0);
			FCEU_DispMessage("State %s loaded.",szFilename);
		}
		else
		{
			//This looks redudant to me... but why bother deleting it:)
			SaveStateStatus[CurrentState]=1;

			FCEU_DispMessage("State %d loaded.",CurrentState);
			SaveStateStatus[CurrentState]=1;
		}
		delete st;

		#ifdef _S9XLUA_H
		if (!internalSaveLoad)
		{
			LuaSaveData saveData;

			char luaSaveFilename [512];
			strncpy(luaSaveFilename, fn, 512);
			luaSaveFilename[512-(1+7/*strlen(".luasav")*/)] = '\0';
			strcat(luaSaveFilename, ".luasav");
			FILE* luaSaveFile = fopen(luaSaveFilename, "rb");
			if(luaSaveFile)
			{
				saveData.ImportRecords(luaSaveFile);
				fclose(luaSaveFile);
			}

			CallRegisteredLuaLoadFunctions(CurrentState, saveData);
		}
		#endif

#ifdef WIN32
	Update_RAM_Search(); // Update_RAM_Watch() is also called.
#endif
		return true;
	}
	else
	{
		if(!fname)
		{
			SaveStateStatus[CurrentState]=1;
		}
		FCEU_DispMessage("Error(s) reading state %d!",CurrentState);
		delete st;
		return 0;
	}
}

void FCEUSS_CheckStates(void)
{
	FILE *st=NULL;
	int ssel;

	for(ssel=0;ssel<10;ssel++)
	{
		st=FCEUD_UTF8fopen(FCEU_MakeFName(FCEUMKF_STATE,ssel,0),"rb");
		if(st)
		{
			SaveStateStatus[ssel]=1;
			fclose(st);
		}
		else
			SaveStateStatus[ssel]=0;
	}

	CurrentState=1;
	StateShow=0;
}

void ResetExState(void (*PreSave)(void), void (*PostSave)(void))
{
	int x;
	for(x=0;x<SFEXINDEX;x++)
	{
		if(SFMDATA[x].desc)
			free(SFMDATA[x].desc);
	}
	// adelikat, 3/14/09:  had to add this to clear out the size parameter.  NROM(mapper 0) games were having savestate crashes if loaded after a non NROM game	because the size variable was carrying over and causing savestates to save too much data
	SFMDATA[0].s = 0;		
	
	SPreSave = PreSave;
	SPostSave = PostSave;
	SFEXINDEX=0;
}

void AddExState(void *v, uint32 s, int type, char *desc)
{
	if(desc)
	{
		SFMDATA[SFEXINDEX].desc=(char *)FCEU_malloc(5);
		strcpy(SFMDATA[SFEXINDEX].desc,desc);
	}
	else
		SFMDATA[SFEXINDEX].desc=0;
	SFMDATA[SFEXINDEX].v=v;
	SFMDATA[SFEXINDEX].s=s;
	if(type) SFMDATA[SFEXINDEX].s|=RLSB;
	if(SFEXINDEX<SFMDATA_SIZE-1)
		SFEXINDEX++;
	else
	{
		static int once=1;
		if(once)
		{
			once=0;
			FCEU_PrintError("Error in AddExState: SFEXINDEX overflow.\nSomebody made SFMDATA_SIZE too small.");
		}
	}
	SFMDATA[SFEXINDEX].v=0;		// End marker.
}

void FCEUI_SelectStateNext(int n)
{
	if(n>0)
		CurrentState=(CurrentState+1)%10;
	else
		CurrentState=(CurrentState+9)%10;
	FCEUI_SelectState(CurrentState, 1);
}

int FCEUI_SelectState(int w, int show)
{
	FCEUSS_CheckStates();
	int oldstate=CurrentState;
	if(w == -1) { StateShow = 0; return 0; } //mbg merge 7/17/06 had to make return a value

	CurrentState=w;
	if(show)
	{
		StateShow=180;
		FCEU_DispMessage("-select state-");
	}
	return oldstate;
}

void FCEUI_SaveState(const char *fname)
{
	if(!FCEU_IsValidUI(FCEUI_SAVESTATE)) return;

	StateShow=0;
	
	FCEUSS_Save(fname);
}

int loadStateFailed = 0; // hack, this function should return a value instead

void FCEUI_LoadState(const char *fname)
{
	if(!FCEU_IsValidUI(FCEUI_LOADSTATE)) return;

	StateShow = 0;
	loadStateFailed = 0;

	/* For network play, be load the state locally, and then save the state to a temporary file,
	and send that.  This insures that if an older state is loaded that is missing some
	information expected in newer save states, desynchronization won't occur(at least not
	from this ;)).
	*/
	BackupLoadState();	//Backup the current state before loading a new one
	
	if (!movie_readonly && autoMovieBackup && freshMovie) //If auto-backup is on, movie has not been altered this session and the movie is in read+write mode
	{
		FCEUI_MakeBackupMovie(false);	//Backup the movie before the contents get altered, but do not display messages						  
	}
	if(FCEUSS_Load(fname))
	{
		//mbg todo netplay
		/*if(FCEUnetplay)
		{
			char *fn = strdup(FCEU_MakeFName(FCEUMKF_NPTEMP, 0, 0).c_str());
			FILE *fp;

			if((fp = fopen(fn," wb")))
			{
				if(FCEUSS_SaveFP(fp,0))
				{
					fclose(fp);
					FCEUNET_SendFile(FCEUNPCMD_LOADSTATE, fn);
				}
				else
				{
					fclose(fp);
				}

				unlink(fn);
			}

			free(fn);
		}*/
		freshMovie = false;		//The movie has been altered so it is no longer fresh
	}
	else
	{
		loadStateFailed = 1;
	}
}

void FCEU_DrawSaveStates(uint8 *XBuf)
{
	if(!StateShow) return;

	FCEU_DrawNumberRow(XBuf,SaveStateStatus,CurrentState);
	StateShow--;
}

//*************************************************************************
//Savestate backup functions
//(Used when making savestates)
//*************************************************************************

string GenerateBackupSaveStateFn(const char *fname)
{
	//This backup is for the backup "slot" for any savestate made.  Example: smb.fc0 becomes smb-bak.fc0
	string filename;
	filename = fname;	//Convert fname to a string object
	int x = filename.find_last_of("."); //Find file extension
	filename.insert(x,"-bak");		//add "-bak" before the dot.  
	
	return filename;
}


void CreateBackupSaveState(const char *fname)
{
	string newFilename = GenerateBackupSaveStateFn(fname);	//Get backup savestate filename
	if (CheckFileExists(newFilename.c_str()))				//See if backup already exists
		remove(newFilename.c_str())	;						//If so, delete it
	rename(fname,newFilename.c_str());						//Rename savestate to backup filename
	undoSS = true;		//There is a backup savestate file to mast last loaded, so undo is possible
}

void SwapSaveState()
{
	//--------------------------------------------------------------------------------------------
	//Both files must exist
	//--------------------------------------------------------------------------------------------
	
	if (!lastSavestateMade) 
	{
		FCEUI_DispMessage("Can't Undo");
		FCEUI_printf("Undo savestate was attempted but unsuccessful because there was not a recently used savestate.\n");
		return;		//If there is no last savestate, can't undo
	}
	string backup = GenerateBackupSaveStateFn(lastSavestateMade);	//Get filename of backup state
	if (!CheckFileExists(backup.c_str())) 
	{
		FCEUI_DispMessage("Can't Undo");
		FCEUI_printf("Undo savestate was attempted but unsuccessful because there was not a backup of the last used savestate.\n");
		return;		//If no backup, can't undo
	}

	//--------------------------------------------------------------------------------------------
	//So both exists, now swap the last savestate and its backup
	//--------------------------------------------------------------------------------------------
	string temp = backup;					//Put backup filename in temp
	temp.append("x");						//Add x
	
	rename(backup.c_str(),temp.c_str());		//rename backup file to temp file
	rename(lastSavestateMade,backup.c_str());	//rename current as backup
	rename(temp.c_str(),lastSavestateMade);		//rename backup as current
	
	undoSS = true;	//Just in case, if this was run, then there is definately a last savestate and backup
	if (redoSS)				//This was a redo function, so if run again it will be an undo again
		redoSS = false;
	else					//This was an undo function so next will be redo, so flag it
		redoSS = true;

	FCEUI_DispMessage("%s restored",backup.c_str());
	FCEUI_printf("%s restored\n",backup.c_str());
}	
	
//------------------------------------------------------------------------------------------------------------------------------------------------------
//*************************************************************************
//Loadstate backup functions
//(Used when Loading savestates)
//*************************************************************************

string GetBackupFileName()
{
	//This backup savestate is a special one specifically made whenever a loadstate occurs so that the user's place in a movie/game is never lost
	//particularly from unintentional loadstating
	string filename;
	int x;
	
	filename = strdup(FCEU_MakeFName(FCEUMKF_STATE,CurrentState,0).c_str());	//Generate normal savestate filename
	x = filename.find_last_of(".");		//Find last dot
	filename = filename.substr(0,x);	//Chop off file extension
	filename.append(".bak.fc0");		//add .bak

	return filename;
}

bool CheckBackupSaveStateExist()
{
	//This function simply checks to see if the backup loadstate exists, the backup loadstate is a special savestate
	//That is made before loading any state, so that the user never loses his data
	string filename = GetBackupFileName(); //Get backup savestate filename
		
	//Check if this filename exists
	fstream test;
	test.open(filename.c_str(),fstream::in);
		
	if (test.fail())
	{
		test.close();
		return false;
	}
	else
	{
		test.close();
		return true;
	}
}

void BackupLoadState()
{
	string filename = GetBackupFileName();
	internalSaveLoad = true;
	FCEUSS_Save(filename.c_str());
	internalSaveLoad = false;
	undoLS = true;
}

void LoadBackup()
{
	if (!undoLS) return;
	string filename = GetBackupFileName();	//Get backup filename
	if (CheckBackupSaveStateExist())
	{
		//internalSaveLoad = true;
		FCEUSS_Load(filename.c_str());		//Load it
		//internalSaveLoad = false;
		redoLS = true;						//Flag redoLoadState
		undoLS = false;						//Flag that LoadBackup cannot be run again
	}
	else
		FCEUI_DispMessage("Error: Could not load %s",filename.c_str());
}

void RedoLoadState()
{
	if (!redoLS) return;
	if (lastLoadstateMade && redoLS)
	{
		FCEUSS_Load(lastLoadstateMade);
		FCEUI_printf("Redoing %s\n",lastLoadstateMade);
	}
	redoLS = false;		//Flag that RedoLoadState can not be run again
	undoLS = true;		//Flag that LoadBackup can be run again
}
