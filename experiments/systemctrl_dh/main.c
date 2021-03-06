#include <pspsdk.h>
#include <pspkernel.h>
#include <pspinit.h>
#include <psputilsforkernel.h>
#include <pspcrypt.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "sysmodpatches.h"
#include "malloc.h"
#include "umd9660_driver.h"
#include "systemctrl_se.h"

PSP_MODULE_INFO("SystemControl", 0x3007, 1, 2);
PSP_MAIN_THREAD_ATTR(0);


int (* sceKernelCheckExecFile)(void *buf, int *check) = (void *)0x88019614;
int (* ProbeExec1)(void *buf, u32 *check)= (void *)0x88019464;
int (* PartitionCheck)(void *st0, void *check) = (void *)0x8805f9b8;
int (* ProbeExec2)(u8 *buf, u32 *check) = (void *)0x88019380;
int (* memlmd_7CF1CD3E)(u8 *buf, int size, int *ret) = (void *)0x8801c6d0;
int (* sceMesgLed_driver_DFF0F308)(u8 *buf, int size, int x) = (void *)0x8801c718;
int (* sceMesgLed_driver_55E4F665)(u8 *buf, int size, int x) = (void *)0x8801c6f0;
int (* FlashfatIoOpen)(PspIoDrvFileArg *arg, char *file, int flags, SceMode mode); 
int (* FlashfatIoRemove)(PspIoDrvFileArg *arg, const char *name); 
int (* FlashfatIoMkdir)(PspIoDrvFileArg *arg, const char *name, SceMode mode); 
int (* FlashfatIoRmdir)(PspIoDrvFileArg *arg, const char *name);
int (* FlashfatIoDopen)(PspIoDrvFileArg *arg, const char *dirname);
int (* FlashfatIoGetstat)(PspIoDrvFileArg *arg, const char *file, SceIoStat *stat);
int (* FlashfatIoChstat)(PspIoDrvFileArg *arg, const char *file, SceIoStat *stat, int bits);
int (* FlashfatIoRename)(PspIoDrvFileArg *arg, const char *oldname, const char *newname); 
int (* FlashfatIoChdir)(PspIoDrvFileArg *arg, const char *dir); 

int sceKernelDcacheWBinvAll();
int sceKernelLoadExecutableObject(char *buf, void *check);
int sceKernelApplyPspRelSection(u32 *a0, void *a1, void *a2, void *a3, void *t0, void *t1);


int debug = 0;

/* ELF file header */
typedef struct { 
	u32		e_magic;
	u8		e_class;
	u8		e_data;
	u8		e_idver;
	u8		e_pad[9];
	u16		e_type; 
	u16		e_machine; 
	u32		e_version; 
	u32		e_entry; 
	u32		e_phoff; 
	u32		e_shoff; 
	u32		e_flags; 
	u16		e_ehsize; 
	u16		e_phentsize; 
	u16		e_phnum; 
	u16		e_shentsize; 
	u16		e_shnum; 
	u16		e_shstrndx; 
} __attribute__((packed)) Elf32_Ehdr;

/* ELF section header */
typedef struct { 
	u32		sh_name; 
	u32		sh_type; 
	u32		sh_flags; 
	u32		sh_addr; 
	u32		sh_offset; 
	u32		sh_size; 
	u32		sh_link; 
	u32		sh_info; 
	u32		sh_addralign; 
	u32		sh_entsize; 
} __attribute__((packed)) Elf32_Shdr;

void ClearCaches()
{
	sceKernelDcacheWBinvAll();
	sceKernelIcacheClearAll();
}

u32 sctrlHENFindFunction(const char* szMod, const char* szLib, u32 nid)
{
	struct SceLibraryEntryTable *entry;
	SceModule *pMod;
	void *entTab;
	int entLen;

	pMod = sceKernelFindModuleByName(szMod);

	if (!pMod)
	{
		//***printf("Cannot find module %s\n", szMod);
		return 0;
	}
	
	int i = 0;

	entTab = pMod->ent_top;
	entLen = pMod->ent_size;
	//***printf("entTab %p - entLen %d\n", entTab, entLen);
	while(i < entLen)
    {
		int count;
		int total;
		unsigned int *vars;

		entry = (struct SceLibraryEntryTable *) (entTab + i);

        if(entry->libname && !strcmp(entry->libname, szLib))
		{
			total = entry->stubcount + entry->vstubcount;
			vars = entry->entrytable;

			if(entry->stubcount > 0)
			{
				for(count = 0; count < entry->stubcount; count++)
				{
					if (vars[count] == nid)
						return vars[count+total];					
				}
			}
		}

		i += (entry->len * 4);
	}

	//***printf("Funtion not found.\n");
	return 0;
}

int IsAddress(void *addr)
{
	u32 u = (u32)addr;

	if (u >= 0x88000000 && u <= 0x883f0000)
		return 1;

	if (u >= 0x08840000 && u <= 0x09FFFFFFF)
		return 1;

	if (u >= 0x08800000 && u <= 0x0883FFFF)
		return 1;

	if (u >= 0x88800000 && u <= 0x8883FFFF)
		return 1;
	
	return 0;
}

int IsStaticElf(void *buf)
{
	Elf32_Ehdr *header = (Elf32_Ehdr *)buf;

	if (header->e_magic == 0x464C457F && header->e_type == 2)
	{
		return 1;
	}

	return 0;
}

int PatchExec2(void *buf, int *check)
{
	int index = check[0x4C/4];

	if (index < 0)
	{
		index += 3;
	}

	u32 addr = (u32)(buf + index);

	if (addr >= 0x88400000 && addr <= 0x88800000)
	{
		return 0;
	}

	check[0x58/4] = ((u32 *)buf)[index / 4] & 0xFFFF;
	return ((u32 *)buf)[index / 4];
}

int PatchExec3(void *buf, int *check, int isPlain, int res)
{
	if (!isPlain)
	{
		/*if (check[0x1C/4] != 0)
		{
			if (IsAddress(check[0x1C/4]))			
				DoModulePatches((void *)check[0x1C/4]);
		}*/		

		return res;
	}

	if ((u32)check[8/4] >= 0x52)
	{
		if (check[0x20/4] == -1);
		{
			if (IsStaticElf(buf))
			{		
				check[0x20/4] = 3;
			}			
		}	
				
		return res;
	}

	if (!(PatchExec2(buf, check) & 0x0000FF00))
	{		
		return res;
	}

	check[0x44/4] = 1;	
	return 0;
}

int PatchExec1(void *buf, int *check)
{
	if (((u32 *)buf)[0] != 0x464C457F) /* ELF */
	{
		return -1;
	}

	if (check[8/4] >= 0x120)
	{
		if (check[8/4] != 0x120 && check[8/4] != 0x141)
			return -1;
		
		if (check[0x10/4] == 0)
		{
			if (check[0x44/4] != 0) 
			{ 
				check[0x48/4] = 1; 
				return 0; 
			} 
			
			return -1;
		}

		check[0x48/4] = 1;
		check[0x44/4] = 1;
		PatchExec2(buf, check);

		return 0;
	}
	else if ((u32)check[8/4] >= 0x52)
	{
		return -1;
	}

	if (check[0x44/4] != 0) 
	{ 
		check[0x48/4] = 1; 
		return 0; 
	} 
	
	return -2;
}

int sceKernelCheckExecFilePatched(void *buf, int *check)
{
	int res = PatchExec1(buf, check);

	if (res == 0)
	{		
		return res;
	}
	
	int isPlain = (((u32 *)buf)[0] == 0x464C457F); /* ELF */

	res = sceKernelCheckExecFile(buf, check);

	return PatchExec3(buf, check, isPlain, res);
}

int ProbeExec1Patched(void *buf, u32 *check)
{
	int res; 
	u16 attr;
	u16 *modinfo;
	u16 realattr;

	res = ProbeExec1(buf, check);

	if (((u32 *)buf)[0] != 0x464C457F)
		return res;

	modinfo = ((u16 *)buf) + (check[0x4C/4] / 2);

	realattr = *modinfo;
	attr = realattr & 0x1E00;
	
	if (attr != 0)
	{
		u16 attr2 = ((u16 *)check)[0x58/2];
		attr2 &= 0x1e00;

		
		if (attr2 != attr)
		{
			((u16 *)check)[0x58/2] = realattr;
		}
	}

	if (check[0x48/4] == 0)
		check[0x48/4] = 1;	

	return res;
}

u32 buf[256/4];

int PartitionCheckPatched(u32 *st0, u32 *check)
{
	SceUID fd = (SceUID)st0[0x34/4];
	u32 pos;
	u16 attributes;

	if (fd < 0)
		return PartitionCheck(st0, check);

	pos = sceIoLseek(fd, 0, PSP_SEEK_CUR);

	if (pos < 0)
		return PartitionCheck(st0, check);

	/* rewind to beginning */
	sceIoLseek(fd, 0, PSP_SEEK_SET);
	if (sceIoRead(fd, buf, 256) < 256)
	{
		sceIoLseek(fd, pos, PSP_SEEK_SET);
		return PartitionCheck(st0, check);
	}

	/* go to module info offset */
	if (buf[0] == 0x50425000) /* PBP */
	{
		sceIoLseek(fd, buf[0x20/4], PSP_SEEK_SET);
		sceIoRead(fd, buf, 4);

		if (buf[0] != 0x464C457F) /* ELF */
		{
			/* Encrypted module */
			sceIoLseek(fd, pos, PSP_SEEK_SET);
			return PartitionCheck(st0, check);
		}

		sceIoLseek(fd, buf[0x20/4]+check[0x4C/4], PSP_SEEK_SET);
	}
	else if (buf[0] == 0x464C457F) /* ELF */
	{
		sceIoLseek(fd, check[0x4C/4], PSP_SEEK_SET);
	}
	else /* encrypted module */
	{
		sceIoLseek(fd, pos, PSP_SEEK_SET);
		return PartitionCheck(st0, check);
	}

	sceIoRead(fd, &attributes, 2);

	if (IsStaticElf(buf))
	{
		check[0x44/4] = 0;
	}
	else
	{
		if (attributes & 0x1000)
		{
			check[0x44/4] = 1;
		}
		else
		{
			check[0x44/4] = 0;
		}
	}

	sceIoLseek(fd, pos, PSP_SEEK_SET);
	return PartitionCheck(st0, check);
}

char *GetStrTab(u8 *buf)
{
	Elf32_Ehdr *header = (Elf32_Ehdr *)buf;
	int i;

	if (header->e_magic != 0x464C457F)
		return NULL;

	u8 *pData = buf+header->e_shoff;

	for (i = 0; i < header->e_shnum; i++)
	{
		if (header->e_shstrndx == i)
		{			
			Elf32_Shdr *section = (Elf32_Shdr *)pData;

			if (section->sh_type == 3)
				return (char *)buf+section->sh_offset;
		}

		pData += header->e_shentsize;

	}

	return NULL;
}
int ProbeExec2Patched(u8 *buf, u32 *check)
{
	int res;	

	res = ProbeExec2(buf, check);

	if (((u32 *)buf)[0] != 0x464C457F)
		return res;

	if (check[0x4C/4] == 0)
	{
		if (IsStaticElf(buf))
		{
			char *strtab = GetStrTab(buf);
		
			if (strtab)
			{
				Elf32_Ehdr *header = (Elf32_Ehdr *)buf;
				int i;

				u8 *pData = buf+header->e_shoff;

				for (i = 0; i < header->e_shnum; i++)
				{
					Elf32_Shdr *section = (Elf32_Shdr *)pData;

					if (strcmp(strtab+section->sh_name, ".rodata.sceModuleInfo") == 0)
					{
						check[0x4C/4] = section->sh_offset;
						check[0x58/4] = 0;
					}
				
					pData += header->e_shentsize;
				}
			}
		}
	}

	return res;
}

int sceKernelApplyPspRelSectionPatched(u32 *a0, void *a1, void *a2, void *a3, void *t0, void *t1)
{
	Elf32_Ehdr *buf = (Elf32_Ehdr *)a0[0];
	int res = sceKernelApplyPspRelSection(a0, a1, a2, a3, t0, t1);
	
	if (buf->e_magic == 0x464C457F) /* ELF */
	{
		if (!IsStaticElf(buf))
		{
			Elf32_Shdr *shdr = (Elf32_Shdr *)(buf->e_phoff + ((u32)buf));
			u32 modinfo_off = shdr->sh_addr;
			char *modinfo = (char *)(modinfo_off + ((u32)buf));

			modinfo = (char *)((u32)modinfo | 0x80000000);

			if (IsAddress(modinfo+4))
			{
				if (sceKernelInitApitype() >= 0)
				{
			
					if ((sceKernelInitApitype() & 0x0200) == 0x0200)
					{			
						if (strcmp(modinfo+4, "vsh_module") == 0)
						{
							PatchVshMain((char *)buf);
						}

						else if (strcmp(modinfo+4, "sysconf_plugin_module") == 0)
						{
							PatchSysconfPlugin((char *)buf);
						}
					}
				}

				if (strcmp(modinfo+4, "sceUmdMan_driver") == 0)
				{
					PatchUmdMan((char *)buf);
				}

				if (strcmp(modinfo+4, "sceMediaSync") == 0)
				{
					PatchInitLoadExecAndMediaSync((char *)buf);
				}

				if (strcmp(modinfo+4, "sceImpose_Driver") == 0)
				{
					OnImposeLoad();
				}				
			}		
		}
	}

	return res;
}

u8 check_keys0[0x10] =
{
	0x71, 0xF6, 0xA8, 0x31, 0x1E, 0xE0, 0xFF, 0x1E,
	0x50, 0xBA, 0x6C, 0xD2, 0x98, 0x2D, 0xD6, 0x2D
}; 

u8 check_keys1[0x10] =
{
	0xAA, 0x85, 0x4D, 0xB0, 0xFF, 0xCA, 0x47, 0xEB,
	0x38, 0x7F, 0xD7, 0xE4, 0x3D, 0x62, 0xB0, 0x10
};

int DecryptSC(u32 *buf, int size)
{
	buf[0] = 5;
	buf[1] = buf[2] = 0;
	buf[3] = 0x100;
	buf[4] = size;

	if (semaphore_4C537C72(buf, size+0x14, buf, size+0x14, 8) < 0)
	{
		return -1;
	}

	return 0;
}

int UnsignCheck(u8 *buf)
{
	u8 enc[0xD0+0x14];
	int iXOR, res;

	memcpy(enc+0x14, buf+0x80, 0xD0);

	for (iXOR = 0; iXOR < 0xD0; iXOR++)
	{
		enc[iXOR+0x14] ^= check_keys1[iXOR&0xF]; 
	}

	if ((res = DecryptSC((u32 *)enc, 0xD0)) < 0)
	{
		return res;
	}

	for (iXOR = 0; iXOR < 0xD0; iXOR++)
	{
		enc[iXOR] ^= check_keys0[iXOR&0xF];
	}

	memcpy(buf+0x80, enc+0x40, 0x90);
	memcpy(buf+0x110, enc, 0x40);

	return 0;
}

u8 temp_sigcheck[0xD0];

int memlmd_7CF1CD3E_Patched(u8 *buf, int size, int *ret)
{
	int unsigncheck = 0;
	int i;
	u32 tag;

	memcpy(temp_sigcheck, buf+0x80, 0xD0);

	for (i = 0; i < 0x58; i++)
	{
		if (buf[0xD4+i] != 0)
		{							
			unsigncheck = 1;
			break;
		}
	}

	if (unsigncheck)
	{
		UnsignCheck(buf);
		
		tag = *(u32 *)&buf[0xD0];

		if (tag != 0x16D59E03)
		{
			memcpy(buf+0x80, temp_sigcheck, 0xD0);
		}
	}

	return memlmd_7CF1CD3E(buf, size, ret);	
}

int sceMesgLed_driver_DFF0F308_Patched(u8 *buf, int size, int x)
{
	int unsigncheck = 0;
	int i;
	u32 tag;

	memcpy(temp_sigcheck, buf+0x80, 0xD0);

	for (i = 0; i < 0x58; i++)
	{
		if (buf[0xD4+i] != 0)
		{							
			unsigncheck = 1;
			break;
		}
	}	

	if (unsigncheck)
	{
		UnsignCheck(buf);	
		
		tag = *(u32 *)&buf[0xD0];

		if (tag != 0x76202403)
		{
			memcpy(buf+0x80, temp_sigcheck, 0xD0);
		}
	}

	return sceMesgLed_driver_DFF0F308(buf, size, x);	
}

int sceMesgLed_driver_55E4F665_Patched(u8 *buf, int size, int x)
{
	int unsigncheck = 0;
	int i;
	u32 tag;

	memcpy(temp_sigcheck, buf+0x80, 0xD0);

	for (i = 0; i < 0x58; i++)
	{
		if (buf[0xD4+i] != 0)
		{							
			unsigncheck = 1;
			break;
		}
	}
	
	if (unsigncheck)
	{
		UnsignCheck(buf);	
		
		tag = *(u32 *)&buf[0xD0];

		if (tag != 0x0F037303)
		{
			memcpy(buf+0x80, temp_sigcheck, 0xD0);
		}
	}

	return sceMesgLed_driver_55E4F665(buf, size, x);	
}

char g_file[256];

char *GetNewPath(char *file)
{
	if (strcmp(file, "/kd") == 0 || strcmp(file, "/KD") == 0)
	{
		strcpy(g_file, file);
		g_file[2] = 'n';
		return g_file;
	}

	if (strcmp(file, "/vsh/module") == 0 || strcmp(file, "/VSH/MODULE") == 0)
	{
		strcpy(g_file, file);
		g_file[5] = 'n';
		return g_file;
	}

	if (strncmp(file, "/kd/", 4) == 0 || strncmp(file, "/KD/", 4) == 0)
	{
		strcpy(g_file, file);
		g_file[2] = 'n';
		return g_file;
	}

	if (strncmp(file, "/vsh/module/", 12) == 0 || strncmp(file, "/VSH/MODULE/", 12) == 0)
	{
		strcpy(g_file, file);
		g_file[5] = 'n';
		return g_file;
	}

	if (strcmp(file, "/k#") == 0 || strcmp(file, "/K#") == 0)
	{
		strcpy(g_file, file);
		g_file[2] = 'd';
		return g_file;
	}

	if (strcmp(file, "/vsh/#odule") == 0 || strcmp(file, "/VSH/#ODULE") == 0)
	{
		strcpy(g_file, file);
		g_file[5] = 'd';
		return g_file;
	}

	if (strncmp(file, "/k#/", 4) == 0 || strncmp(file, "/K#/", 4) == 0)
	{
		strcpy(g_file, file);
		g_file[2] = 'd';
		return g_file;
	}

	if (strncmp(file, "/vsh/#odule/", 12) == 0 || strncmp(file, "/VSH/#ODULE/", 12) == 0)
	{
		strcpy(g_file, file);
		g_file[5] = 'd';
		return g_file;
	}

	return file;
}

int FlashfatIoOpen_Patched(PspIoDrvFileArg *arg, char *file, int flags, SceMode mode)
{
	if (file)
	{
		return FlashfatIoOpen(arg, GetNewPath(file), flags, mode);
	}

	return FlashfatIoOpen(arg, file, flags, mode);
}

int FlashfatIoRemove_Patched(PspIoDrvFileArg *arg, const char *name)
{
	if (name)
	{
		return FlashfatIoRemove(arg, GetNewPath((char *)name));
	}

	return FlashfatIoRemove(arg, name);
}

int FlashfatIoMkdir_Patched(PspIoDrvFileArg *arg, const char *name, SceMode mode)
{
	if (name)
	{
		return FlashfatIoMkdir(arg, GetNewPath((char *)name), mode);
	}

	return FlashfatIoMkdir(arg, name, mode);
}

int FlashfatIoRmdir_Patched(PspIoDrvFileArg *arg, const char *name)
{
	if (name)
	{
		return FlashfatIoRmdir(arg, GetNewPath((char *)name));
	}

	return FlashfatIoRmdir(arg, name);
}

int FlashfatIoDopen_Patched(PspIoDrvFileArg *arg, const char *dirname)
{
	if (dirname)
	{
		return FlashfatIoDopen(arg, GetNewPath((char *)dirname));
	}

	return FlashfatIoDopen(arg, dirname);
}

int FlashfatIoGetstat_Patched(PspIoDrvFileArg *arg, const char *file, SceIoStat *stat)
{
	if (file)
	{
		return FlashfatIoGetstat(arg, GetNewPath((char *)file), stat);
	}

	return FlashfatIoGetstat(arg, file, stat);
}

int FlashfatIoChstat_Patched(PspIoDrvFileArg *arg, const char *file, SceIoStat *stat, int bits)
{
	if (file)
	{
		return FlashfatIoChstat(arg, GetNewPath((char *)file), stat, bits);
	}

	return FlashfatIoChstat(arg, file, stat, bits);
}

int FlashfatIoRename_Patched(PspIoDrvFileArg *arg, const char *oldname, const char *newname)
{
	if (oldname)
	{
		if (newname)
		{
			return FlashfatIoRename(arg, GetNewPath((char *)oldname), GetNewPath((char *)newname));
		}

		return FlashfatIoRename(arg, GetNewPath((char *)oldname), newname);
	}

	if (newname)
		return FlashfatIoRename(arg, oldname, GetNewPath((char *)newname));

	return FlashfatIoRename(arg, oldname, newname);
}

int FlashfatIoChdir_Patched(PspIoDrvFileArg *arg, const char *dir)
{
	if (dir)
	{
		return FlashfatIoChdir(arg, GetNewPath((char *)dir));
	}

	return FlashfatIoChdir(arg, dir);
}

int sceIoAddDrvPatched(PspIoDrv *drv)
{
	if (drv->name)
	{
		if (strcmp(drv->name, "flashfat") == 0)
		{
			FlashfatIoOpen = drv->funcs->IoOpen;
			FlashfatIoRemove = drv->funcs->IoRemove;
			FlashfatIoMkdir = drv->funcs->IoMkdir;
			FlashfatIoRmdir = drv->funcs->IoRmdir;
			FlashfatIoDopen = drv->funcs->IoDopen;
			FlashfatIoGetstat = drv->funcs->IoGetstat;
			FlashfatIoChstat = drv->funcs->IoChstat;
			FlashfatIoRename = drv->funcs->IoRename;
			FlashfatIoChdir = drv->funcs->IoChdir;
			drv->funcs->IoOpen = FlashfatIoOpen_Patched;
			drv->funcs->IoRemove = FlashfatIoRemove_Patched;
			drv->funcs->IoMkdir = FlashfatIoMkdir_Patched;
			drv->funcs->IoRmdir = FlashfatIoRmdir_Patched;
			drv->funcs->IoDopen = FlashfatIoDopen_Patched;
			drv->funcs->IoGetstat = FlashfatIoGetstat_Patched;
			drv->funcs->IoChstat = FlashfatIoChstat_Patched;
			drv->funcs->IoRename = FlashfatIoRename_Patched;
			drv->funcs->IoChdir = FlashfatIoChdir_Patched;
		}		
	}
	
	return sceIoAddDrv(drv);
}

void PatchSyscall(u32 funcaddr, void *newfunc)
{
	u32 *vectors = (u32 *)0x88029c30;

	int i;

	for (i = 0; i < 0x1000; i++)
	{
		if (vectors[i] == funcaddr)
		{
			vectors[i] = (u32)newfunc;
		}
	}
} 

int WriteFile(char *file, void *buf, int size)
{
	SceUID fd = sceIoOpen(file, PSP_O_CREAT | PSP_O_TRUNC | PSP_O_WRONLY, 0777);

	if (fd >= 0)
	{
		sceIoWrite(fd, buf, size);
		sceIoClose(fd);
		return 1;
	}

	return -1;
}

int w;

void UndoSuperNoPlainModuleCheckPatch()
{
	_sw((u32)sceKernelCheckExecFile, 0x8801c9ac);
	MAKE_CALL(0x88017EC4, sceKernelCheckExecFile);
	MAKE_CALL(0x88017F14, sceKernelCheckExecFile);
	MAKE_CALL(0x88019CC0, sceKernelCheckExecFile);
	_sw(w, 0x8801D05C);

	MAKE_CALL(0x88019b50, ProbeExec1);
	MAKE_CALL(0x880199b0, ProbeExec2);
	_sw(0x11c0ffb2, 0x88019ac0);	

	MAKE_JUMP(0x8805FFB4, sceKernelCheckExecFile);
	MAKE_CALL(0x8805da24, PartitionCheck);

	ClearCaches();
}

#define JR_RA_INSN 0x03e00008
#define MOVE_V0_0_INSN 0x00001021

int module_start(SceSize args, void *argp)
{
	/* patch applypsprelsection to patch the rest of modules after relocation */
	MAKE_CALL(0x8801a3b0, sceKernelApplyPspRelSectionPatched);
	MAKE_CALL(0x8801a5c0, sceKernelApplyPspRelSectionPatched);
	
	/* Patch calls and references to sceKernelCheckExecFile */
	_sw((u32)sceKernelCheckExecFilePatched, 0x8801c9ac);
	MAKE_CALL(0x88017EC4, sceKernelCheckExecFilePatched);
	MAKE_CALL(0x88017F14, sceKernelCheckExecFilePatched);
	MAKE_CALL(0x88019CC0, sceKernelCheckExecFilePatched);
	w = _lw(0x8801D05C);
	_sw(_lw(0x8801D040), 0x8801D05C);

	/* Patch 2 functions called by sceKernelProbeExecutableObject */
	MAKE_CALL(0x88019b50, ProbeExec1Patched);
	MAKE_CALL(0x880199b0, ProbeExec2Patched);

	// some sceKernelProbeExecutableObject patches (elf)
	// new
	_sw(0x11c00015, 0x88019ac0);
	// old (buggy)
	//_sw(0x11c0ffb5, 0x88019ac0);
	//_sw(0x3c030000, 0x88019ac4);

	/* For load sign checked modules */
	MAKE_CALL(0x8801b0c4, memlmd_7CF1CD3E_Patched);
	MAKE_CALL(0x8801b160, sceMesgLed_driver_DFF0F308_Patched);
	MAKE_CALL(0x8801b11c, sceMesgLed_driver_55E4F665_Patched);

	
	ClearCaches();

	/* Patch ModuleMgr */
	_sw(JR_RA_INSN, 0x8805FF4C);
	_sw(MOVE_V0_0_INSN, 0x8805FF50);
	MAKE_JUMP(0x8805FFB4, sceKernelCheckExecFilePatched);
	MAKE_CALL(0x8805da24, PartitionCheckPatched);
		
	/* Patch IoFileMgr sceIoAddDrv nid */
	_sw((u32)sceIoAddDrvPatched, 0x88050d9c);
	ClearCaches();
		
	return 0;
}

int module_stop(void)
{
	return 0;
}

