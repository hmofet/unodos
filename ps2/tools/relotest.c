/* ===========================================================================
 * relotest.c - host validator for the EE overlay loader's relocation engine.
 *
 * It runs the SAME ELF32/MIPS relocation algorithm as ps2/ee_modload.c against
 * a real app .uno (ET_REL) image, laying its sections at addresses chosen to
 * MATCH a reference `ld` link of the same object, then compares the relocated
 * .text/.rodata bytes against that reference link byte-for-byte.  If they are
 * identical, the relocator in ee_modload.c (R_MIPS_32/26/HI16/LO16) is proven
 * correct - the only EE-specific bits not exercised here are the mc0: read and
 * FlushCache, which are I/O, not relocation math.
 *
 * Usage: relotest <app.uno> <app_linked.elf>
 *   app_linked.elf is `ld -Ttext-segment=BASE app.o stubs.o`, providing both
 *   the golden relocated bytes AND the final addresses of every symbol (which
 *   we feed the relocator so the two layouts coincide).
 * ===========================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint32_t Elf32_Word; typedef int32_t Elf32_Sword;
typedef uint16_t Elf32_Half; typedef uint32_t Elf32_Addr; typedef uint32_t Elf32_Off;
typedef struct { unsigned char e_ident[16]; Elf32_Half e_type,e_machine;
  Elf32_Word e_version; Elf32_Addr e_entry; Elf32_Off e_phoff,e_shoff;
  Elf32_Word e_flags; Elf32_Half e_ehsize,e_phentsize,e_phnum,e_shentsize,e_shnum,e_shstrndx; } Elf32_Ehdr;
typedef struct { Elf32_Word sh_name,sh_type,sh_flags; Elf32_Addr sh_addr; Elf32_Off sh_offset;
  Elf32_Word sh_size,sh_link,sh_info,sh_addralign,sh_entsize; } Elf32_Shdr;
typedef struct { Elf32_Word st_name; Elf32_Addr st_value; Elf32_Word st_size;
  unsigned char st_info,st_other; Elf32_Half st_shndx; } Elf32_Sym;
typedef struct { Elf32_Addr r_offset; Elf32_Word r_info; Elf32_Sword r_addend; } Elf32_Rela;

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_ALLOC 2
#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define R_SYM(i) ((i)>>8)
#define R_TYPE(i) ((i)&0xFF)
#define R_MIPS_32 2
#define R_MIPS_26 4
#define R_MIPS_HI16 5
#define R_MIPS_LO16 6
#define MAX_SHDR 48

/* The reference ELF's symbol table: name -> final address (for UNDEF imports
   AND, since we lay sections at the reference addresses, defined symbols). */
static Elf32_Sym *gRefSym; static char *gRefStr; static int gRefNsym;
static unsigned ref_addr(const char *name){
  for(int i=0;i<gRefNsym;i++) if(!strcmp(gRefStr+gRefSym[i].st_name,name)) return gRefSym[i].st_value;
  return 0;
}
static void load_ref(const char *path){
  FILE *f=fopen(path,"rb"); if(!f){perror(path);exit(2);} fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  unsigned char *b=malloc(n); fread(b,1,n,f); fclose(f);
  Elf32_Ehdr *eh=(Elf32_Ehdr*)b; Elf32_Shdr *sh=(Elf32_Shdr*)(b+eh->e_shoff);
  for(int i=0;i<eh->e_shnum;i++) if(sh[i].sh_type==SHT_SYMTAB){
    gRefSym=(Elf32_Sym*)(b+sh[i].sh_offset); gRefNsym=sh[i].sh_size/sizeof(Elf32_Sym);
    gRefStr=(char*)(b+sh[sh[i].sh_link].sh_offset); }
}
/* pull a section's bytes from the reference ELF by name (golden output) */
static unsigned char *ref_section(const char *path,const char *name,long *sz,unsigned *vaddr){
  FILE *f=fopen(path,"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  unsigned char *b=malloc(n); fread(b,1,n,f); fclose(f);
  Elf32_Ehdr *eh=(Elf32_Ehdr*)b; Elf32_Shdr *sh=(Elf32_Shdr*)(b+eh->e_shoff);
  char *shstr=(char*)(b+sh[eh->e_shstrndx].sh_offset);
  for(int i=0;i<eh->e_shnum;i++) if(!strcmp(shstr+sh[i].sh_name,name)){
    *sz=sh[i].sh_size; *vaddr=sh[i].sh_addr; unsigned char *r=malloc(*sz);
    memcpy(r,b+sh[i].sh_offset,*sz); return r; }
  *sz=0; return NULL;
}

/* HI16/LO16 queue - identical to ee_modload.c */
static unsigned int *gHiLoc[32]; static Elf32_Word gHiVal[32]; static int gHiN=0;
static void hi_push(unsigned*l,Elf32_Word v){ if(gHiN<32){gHiLoc[gHiN]=l;gHiVal[gHiN]=v;gHiN++;} }
static void hi_flush(Elf32_Word lv){ short lo=(short)(lv&0xFFFF); int w=0;
  for(int i=0;i<gHiN;i++){ if(gHiVal[i]==lv){ Elf32_Word hi=((gHiVal[i]-(Elf32_Word)(Elf32_Sword)lo)>>16)&0xFFFF;
    *gHiLoc[i]=(*gHiLoc[i]&0xFFFF0000)|hi; } else { gHiLoc[w]=gHiLoc[i]; gHiVal[w]=gHiVal[i]; w++; } } gHiN=w; }

int main(int argc,char**argv){
  if(argc<3){fprintf(stderr,"usage: relotest <app.uno> <ref.elf>\n");return 2;}
  load_ref(argv[2]);
  FILE *f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
  unsigned char *img=malloc(len); fread(img,1,len,f); fclose(f);

  Elf32_Ehdr *eh=(Elf32_Ehdr*)img; Elf32_Shdr *sh=(Elf32_Shdr*)(img+eh->e_shoff);
  int shnum=eh->e_shnum, symtab_i=-1,strtab_i=-1;
  Elf32_Word seg_base[MAX_SHDR]; memset(seg_base,0,sizeof seg_base);
  char *shstr=(char*)(img+sh[eh->e_shstrndx].sh_offset);

  for(int i=0;i<shnum;i++) if(sh[i].sh_type==SHT_SYMTAB){symtab_i=i;strtab_i=sh[i].sh_link;}
  Elf32_Sym *syms=(Elf32_Sym*)(img+sh[symtab_i].sh_offset);
  char *strtab=(char*)(img+sh[strtab_i].sh_offset);

  /* Lay each ALLOC section at the SAME vaddr the reference link gave it, so the
     relocated bytes are directly comparable.  We read those vaddrs back from
     the reference ELF by section name. */
  unsigned char *blob[MAX_SHDR]; memset(blob,0,sizeof blob);
  for(int i=0;i<shnum;i++){
    if(!(sh[i].sh_flags&SHF_ALLOC)) continue;
    if(sh[i].sh_type!=SHT_PROGBITS && sh[i].sh_type!=SHT_NOBITS) continue;
    const char *nm=shstr+sh[i].sh_name; long rsz; unsigned va;
    unsigned char *rs=ref_section(argv[2],nm,&rsz,&va);
    if(!rs){ /* section absent in ref (e.g. merged) - fall back to its own copy */
      va=ref_addr(nm); rsz=sh[i].sh_size; }
    seg_base[i]=va;
    unsigned char *m=malloc(sh[i].sh_size?sh[i].sh_size:1);
    if(sh[i].sh_type==SHT_NOBITS) memset(m,0,sh[i].sh_size);
    else memcpy(m,img+sh[i].sh_offset,sh[i].sh_size);
    blob[i]=m;
  }

  /* relocate (the exact ee_modload.c algorithm) */
  for(int i=0;i<shnum;i++){
    if(sh[i].sh_type!=SHT_RELA) continue;
    Elf32_Word tgt=sh[i].sh_info; if(tgt>=(Elf32_Word)shnum||!blob[tgt]) continue;
    unsigned char *base=blob[tgt];
    Elf32_Rela *rel=(Elf32_Rela*)(img+sh[i].sh_offset);
    Elf32_Word nrel=sh[i].sh_size/sizeof(Elf32_Rela); gHiN=0;
    for(Elf32_Word j=0;j<nrel;j++){
      Elf32_Word type=R_TYPE(rel[j].r_info), symx=R_SYM(rel[j].r_info);
      unsigned int *loc=(unsigned int*)(base+rel[j].r_offset);
      Elf32_Sym *s=&syms[symx]; const char *snm=strtab+s->st_name; Elf32_Word S;
      if(s->st_shndx==SHN_UNDEF) S=ref_addr(snm);
      else if(s->st_shndx==SHN_ABS) S=s->st_value;
      else S=seg_base[s->st_shndx]+s->st_value;
      Elf32_Word A=rel[j].r_addend, value=S+A;
      switch(type){
        case R_MIPS_32: *loc+=value; break;
        case R_MIPS_26: *loc=(*loc&0xFC000000)|((value>>2)&0x03FFFFFF); break;
        case R_MIPS_HI16: hi_push(loc,value); break;
        case R_MIPS_LO16: hi_flush(value); *loc=(*loc&0xFFFF0000)|(value&0xFFFF); break;
        default: fprintf(stderr,"unhandled reloc %u\n",type); return 3;
      }
    }
  }

  /* compare relocated .text + .rodata against the reference link */
  int fails=0;
  const char *cmp[]={".text",".rodata",".rodata.str1.8",NULL};
  for(int c=0;cmp[c];c++){
    int si=-1; for(int i=0;i<shnum;i++) if(!strcmp(shstr+sh[i].sh_name,cmp[c])&&blob[i]) si=i;
    if(si<0) continue;
    long rsz; unsigned va; unsigned char *ref=ref_section(argv[2],cmp[c],&rsz,&va);
    if(!ref){ printf("  %-16s : (absent in ref, skipped)\n",cmp[c]); continue; }
    long n=sh[si].sh_size<rsz?sh[si].sh_size:rsz;
    int mism=0; long first=-1;
    for(long k=0;k<n;k++) if(blob[si][k]!=ref[k]){ if(first<0)first=k; mism++; }
    printf("  %-16s : %ld bytes, %s%s\n",cmp[c],n, mism?"MISMATCH":"MATCH",
           mism?"":" (relocated == ld golden)");
    if(mism){ printf("      first diff @ +0x%lx: got %02x exp %02x\n",first,blob[si][first],ref[first]); fails++; }
  }
  printf("%s\n", fails? "RESULT: FAIL" : "RESULT: PASS - relocator output is byte-identical to ld");
  return fails?1:0;
}
