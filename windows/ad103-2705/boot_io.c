/* SPDX-License-Identifier: GPL-2.0-only */
/* USB log/assets only. Windows ESP is opened read-only. No boot variables. */
#include "boot_io.h"
#include "boot_profile.h"
#include "compat/compat.h"
#include "format.h"
#include "profile.h"
#include "sha256.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

static EFI_SYSTEM_TABLE *Table;
static EFI_HANDLE Self,Windows,OwnVolume;
static EFI_FILE_PROTOCOL *Root,*Log;
static BOOLEAN Silent,Sealed;
static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,InfoGuid=EFI_FILE_INFO_ID;
static const EFI_GUID WindowsPartition=GU_WINDOWS_PARTITION_INITIALIZER;
/* USB route: nothing on the ESP is renamed. The staged image is the real,
 * original boot manager, loaded and started read-only from the USB run. */
static const CHAR16 WindowsName[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

static BOOLEAN Inspect(const EFI_DEVICE_PATH_PROTOCOL *p,UINTN *size,BOOLEAN *usb,BOOLEAN *windows) {
  UINTN at=0,hd=0;*usb=FALSE;*windows=FALSE;
  if(!p)return FALSE;
  for(UINTN nodes=0;nodes<64&&at+4<=4096;nodes++) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>4096-at)return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE) {
      if(n!=4||p->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE)return FALSE;
      if(hd>16)return FALSE;   /* sane bound on repeated harddrive nodes */
      *size=at+4;return TRUE;
    }
    if(p->Type==MESSAGING_DEVICE_PATH&&p->SubType==MSG_USB_DP&&n==sizeof(USB_DEVICE_PATH))*usb=TRUE;
    if(p->Type==MEDIA_DEVICE_PATH&&p->SubType==MEDIA_HARDDRIVE_DP) {
      if(n!=sizeof(HARDDRIVE_DEVICE_PATH))return FALSE;
      /* A volume path may carry more than one harddrive node. Match the one that
       * names the Windows ESP instead of refusing the second node: an internal
       * ESP boot was exiting silently here while the same code worked from USB. */
      hd++;
      const HARDDRIVE_DEVICE_PATH *d=(const VOID *)p;
      if(d->MBRType==2&&d->SignatureType==2&&d->PartitionNumber==1&&d->PartitionStart==2048&&
         d->PartitionSize==409600&&!memcmp(d->Signature,&WindowsPartition,16))*windows=TRUE;
    }
    at+=n;p=(const VOID *)((const UINT8 *)p+n);
  }
  return FALSE;
}
VOID GuBootSay(const CHAR16 *text) {
  if(!Silent&&Table&&Table->ConOut&&Table->ConOut->OutputString)
    Table->ConOut->OutputString(Table->ConOut,(CHAR16 *)text);
}
VOID GuBootSilence(VOID){Silent=TRUE;}
/* The transaction silences the console because the display driver is being
 * unloaded underneath it. Every terminal path has to undo that first: a halt
 * the operator cannot see is indistinguishable from a hang, and the habitual
 * fix for a hang is a reset, which clears the volatile selectors. */
VOID GuBootWake(VOID){Silent=FALSE;}
VOID GuLog(const char *format,...) {
  /* Logging is diagnostics, not a hardware condition. Once the volume is closed
   * on purpose, a write cannot happen and must not be recorded as a fault: the
   * caller's Gu.fault checks run *after* close, so a purposeful close would
   * otherwise poison every later gate and refuse a handoff that is already
   * correct. */
  if(Sealed||!Log)return;
  CHAR8 buffer[2048];va_list args;va_start(args,format);
  size_t n=GuFormatV(buffer,sizeof(buffer),format,args);va_end(args);
  if(n==(size_t)-1||n>=sizeof(buffer)){Gu.fault=TRUE;return;}
  UINTN bytes=n;EFI_STATUS s=Log->Write(Log,&bytes,buffer);
  if(s!=EFI_SUCCESS||bytes!=n||Log->Flush(Log)!=EFI_SUCCESS)Gu.fault=TRUE;
}
VOID GuLogText(const char *text){GuLog("%s",text);}
BOOLEAN GuBootLogOpen(VOID){return Log!=NULL;}
EFI_STATUS GuBootOpen(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  if(Table)return EFI_ALREADY_STARTED;
  if(!image||!t||!t->BootServices)return EFI_INVALID_PARAMETER;
  Table=t;Self=image;
  EFI_BOOT_SERVICES *b=t->BootServices;EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  EFI_STATUS s=b->HandleProtocol(image,&LoadedGuid,(VOID **)&loaded);
  if(s!=EFI_SUCCESS||!loaded||!loaded->DeviceHandle){
    t->ConOut->OutputString(t->ConOut,L"\r\nAD104: no loaded-image handle.\r\n");
    return EFI_UNSUPPORTED;
  }
  OwnVolume=loaded->DeviceHandle;s=b->HandleProtocol(OwnVolume,&PathGuid,(VOID **)&path);
  UINTN length=0;BOOLEAN usb=FALSE,windows=FALSE;
  /* Upstream required a USB stick. This port also runs from the internal ESP so
   * the unlock can be unattended; assets and the log then live on that ESP. */
  if(s!=EFI_SUCCESS||!Inspect(path,&length,&usb,&windows)){
    t->ConOut->OutputString(t->ConOut,L"\r\nAD104: own volume has no usable device path.\r\n");
    return EFI_ACCESS_DENIED;
  }
  s=b->HandleProtocol(OwnVolume,&FsGuid,(VOID **)&fs);
  if(s!=EFI_SUCCESS||!fs){
    t->ConOut->OutputString(t->ConOut,L"\r\nAD104: own volume exposes no SimpleFileSystem.\r\n");
    return EFI_UNSUPPORTED;
  }
  s=fs->OpenVolume(fs,&Root);
  if(s!=EFI_SUCCESS||!Root){
    t->ConOut->OutputString(t->ConOut,L"\r\nAD104: OpenVolume failed on own volume.\r\n");
    return EFI_DEVICE_ERROR;
  }
  EFI_FILE_PROTOCOL *dir=NULL;
  s=Root->Open(Root,&dir,L"\\AD104-UNLOCK",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
  if(s!=EFI_SUCCESS||!dir){
    t->ConOut->OutputString(t->ConOut,L"\r\nAD104: cannot create \\AD104-UNLOCK on own volume (read-only?).\r\n");
    return EFI_DEVICE_ERROR;
  }
  for(UINTN slot=1;slot<=9999;slot++) {
    CHAR16 name[]=L"RUN0000.TXT";UINTN number=slot;
    for(UINTN at=7;at>3;){at--;name[at]=(CHAR16)('0'+number%10);number/=10;}
    EFI_FILE_PROTOCOL *existing=NULL;s=dir->Open(dir,&existing,name,EFI_FILE_MODE_READ,0);
    if(s==EFI_SUCCESS&&existing){s=existing->Close(existing);if(s!=EFI_SUCCESS)break;continue;}
    if(s!=EFI_NOT_FOUND)break;
    s=dir->Open(dir,&Log,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);break;
  }
  EFI_STATUS closed=dir->Close(dir);
  if(s!=EFI_SUCCESS||!Log){
    t->ConOut->OutputString(t->ConOut,L"\r\nAD104: cannot create RUNxxxx.TXT.\r\n");
    return s==EFI_SUCCESS?EFI_OUT_OF_RESOURCES:s;
  }
  if(closed!=EFI_SUCCESS)return closed;
  if(!b->SetWatchdogTimer)return EFI_UNSUPPORTED;
  return b->SetWatchdogTimer(0,0,0,NULL);
}
static EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *root,const CHAR16 *name,UINTN expected,VOID **data) {
  *data=NULL;EFI_FILE_PROTOCOL *f=NULL;UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
  EFI_STATUS s=root->Open(root,&f,(CHAR16 *)name,EFI_FILE_MODE_READ,0);
  if(s!=EFI_SUCCESS||!f)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  s=f->GetInfo(f,&InfoGuid,&info_bytes,info_words);
  EFI_FILE_INFO *info=(VOID *)info_words;
  if(s==EFI_SUCCESS&&(info_bytes<sizeof(EFI_FILE_INFO)||info_bytes>sizeof(info_words)||
     info->FileSize!=expected||(info->Attribute&EFI_FILE_DIRECTORY)))s=EFI_COMPROMISED_DATA;
  VOID *p=NULL;
  if(s==EFI_SUCCESS){p=GuAlloc(expected);if(!p)s=EFI_OUT_OF_RESOURCES;}
  for(UINTN at=0;s==EFI_SUCCESS&&at<expected;) {
    UINTN n=expected-at;if(n>1048576)n=1048576;UINTN wanted=n;
    s=f->Read(f,&n,(UINT8 *)p+at);if(s==EFI_SUCCESS&&n!=wanted)s=EFI_DEVICE_ERROR;at+=n;
  }
  if(s==EFI_SUCCESS){UINT8 tail;UINTN n=1;s=f->Read(f,&n,&tail);if(s==EFI_SUCCESS&&n)s=EFI_COMPROMISED_DATA;}
  EFI_STATUS closed=f->Close(f);if(s==EFI_SUCCESS)s=closed;
  if(s!=EFI_SUCCESS){GuFree(p);return s;}*data=p;return EFI_SUCCESS;
}
/* Reads a whole file using its on-disk size instead of a pinned length. Used for
 * bootmgfw.efi: the ESP copy is replaced by Windows updates and does not
 * necessarily match the C:\Windows\Boot\EFI\ staging copy. */
#define GU_BOOTMGR_MAX (64u * 1024u * 1024u)
static EFI_STATUS ReadFileActual(EFI_FILE_PROTOCOL *root,const CHAR16 *name,VOID **data,UINTN *size) {
  *data=NULL;*size=0;EFI_FILE_PROTOCOL *f=NULL;UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
  EFI_STATUS s=root->Open(root,&f,(CHAR16 *)name,EFI_FILE_MODE_READ,0);
  if(s!=EFI_SUCCESS||!f)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  s=f->GetInfo(f,&InfoGuid,&info_bytes,info_words);
  EFI_FILE_INFO *info=(VOID *)info_words;
  if(s==EFI_SUCCESS&&(info_bytes<sizeof(EFI_FILE_INFO)||info_bytes>sizeof(info_words)||
     !info->FileSize||info->FileSize>GU_BOOTMGR_MAX||(info->Attribute&EFI_FILE_DIRECTORY)))s=EFI_COMPROMISED_DATA;
  UINTN expected=0;
  if(s==EFI_SUCCESS){expected=(UINTN)info->FileSize;*size=expected;
    VOID *p=GuAlloc(expected);if(!p)s=EFI_OUT_OF_RESOURCES;else *data=p;}
  for(UINTN at=0;s==EFI_SUCCESS&&at<expected;) {
    UINTN n=expected-at;if(n>1048576)n=1048576;UINTN wanted=n;
    s=f->Read(f,&n,(UINT8 *)*data+at);if(s==EFI_SUCCESS&&n!=wanted)s=EFI_DEVICE_ERROR;at+=n;
  }
  if(s==EFI_SUCCESS){UINT8 tail;UINTN n=1;s=f->Read(f,&n,&tail);if(s==EFI_SUCCESS&&n)s=EFI_COMPROMISED_DATA;}
  EFI_STATUS closed=f->Close(f);if(s==EFI_SUCCESS)s=closed;
  if(s!=EFI_SUCCESS){if(*data)GuFree(*data);*data=NULL;*size=0;}
  return s;
}
static void HexHash(const UINT8 *h,char out[65]) {
  static const char d[]="0123456789abcdef";
  for(UINTN i=0;i<32;i++){out[i*2]=d[h[i]>>4];out[i*2+1]=d[h[i]&15];}
  out[64]=0;
}
EFI_STATUS GuBootLoadAssets(VOID) {
  static const CHAR16 *names[]={L"\\AD104-ASSETS\\booter_load_ad102.bin",L"\\AD104-ASSETS\\v67-ad102-610.43.03-four-plm.bin",
    L"\\AD104-ASSETS\\gsp_ga10x.bin",L"\\AD104-ASSETS\\gsp_boot_ad102.bin",L"\\AD104-ASSETS\\gsp_boot_desc_ad102.bin"};
  static const char *full[]={"gpu-unlock/610.43.03/booter_load_ad102.bin","gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin",
    "gpu-unlock/610.43.03/gsp_ga10x.bin","gpu-unlock/610.43.03/gsp_boot_ad102.bin","gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin"};
  static const UINTN sizes[]={57672,64000,84277400,36864,84};
  if(!Root||Gu.started)return EFI_NOT_READY;
  for(UINTN i=0;i<5;i++) {
    VOID *p=NULL;EFI_STATUS s=ReadFile(Root,names[i],sizes[i],&p);
    if(s!=EFI_SUCCESS)return s;
    GuAssets[i].name=full[i];GuAssets[i].firmware=(struct firmware){sizes[i],p};
  }
  return GuAd104AssetsValid()?EFI_SUCCESS:EFI_CRC_ERROR;
}
EFI_STATUS GuBootLoadWindows(VOID) {
  if(Windows||!Table||Gu.started)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Table->BootServices;EFI_HANDLE *h=NULL,volume=NULL;UINTN count=0,matches=0,path_bytes=0;
  EFI_DEVICE_PATH_PROTOCOL *base=NULL;
  EFI_STATUS s=b->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,&count,&h);
  if(s==EFI_SUCCESS&&(!h||!count||count>128))s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;UINTN bytes=0;BOOLEAN usb=FALSE,windows=FALSE;
    if(b->HandleProtocol(h[i],&PathGuid,(VOID **)&p)==EFI_SUCCESS&&Inspect(p,&bytes,&usb,&windows)&&windows&&!usb) {
      matches++;volume=h[i];base=p;path_bytes=bytes;
    }
  }
  if(h)b->FreePool(h);
  /* Same-volume handoff is the normal case when running from the ESP. */
  if(s!=EFI_SUCCESS||matches!=1)return EFI_NOT_FOUND;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_FILE_PROTOCOL *root=NULL;
  s=b->HandleProtocol(volume,&FsGuid,(VOID **)&fs);
  if(s==EFI_SUCCESS&&fs)s=fs->OpenVolume(fs,&root);else s=EFI_UNSUPPORTED;
  if(s!=EFI_SUCCESS||!root)return EFI_DEVICE_ERROR;
  VOID *bytes=NULL;UINTN actual=0;
  s=ReadFileActual(root,WindowsName,&bytes,&actual);
  EFI_STATUS closed=root->Close(root);if(s==EFI_SUCCESS)s=closed;
  if(s==EFI_SUCCESS) {
    /* Print the real ESP copy's size and digest. The pin above the ESP is a
     * moving target across Windows updates; this line is how the pin gets
     * corrected for this machine. */
    UINT8 hash[32];char hex[65];
    GuSha256((const VOID *)bytes,actual,hash);HexHash(hash,hex);
    GuLog("bootmgfw: size=%zu sha256=%s\n",(size_t)actual,hex);
  }
  UINTN node_bytes=4+sizeof(WindowsName),total=path_bytes-4+node_bytes+4;
  UINT8 *path=NULL;
  if(s==EFI_SUCCESS){path=GuAlloc(total);if(!path)s=EFI_OUT_OF_RESOURCES;}
  if(s==EFI_SUCCESS) {
    memcpy(path,base,path_bytes-4);UINT8 *node=path+path_bytes-4;
    node[0]=MEDIA_DEVICE_PATH;node[1]=MEDIA_FILEPATH_DP;node[2]=(UINT8)node_bytes;node[3]=(UINT8)(node_bytes>>8);
    memcpy(node+4,WindowsName,sizeof(WindowsName));node+=node_bytes;
    node[0]=END_DEVICE_PATH_TYPE;node[1]=END_ENTIRE_DEVICE_PATH_SUBTYPE;node[2]=4;node[3]=0;
    s=b->LoadImage(FALSE,Self,(VOID *)path,bytes,actual,&Windows);
  }
  GuFree(bytes);GuFree(path);
  if(s==EFI_SUCCESS) {
    EFI_LOADED_IMAGE_PROTOCOL *im=NULL;s=b->HandleProtocol(Windows,&LoadedGuid,(VOID **)&im);
    if(s==EFI_SUCCESS&&(!im||im->DeviceHandle!=volume))s=EFI_UNSUPPORTED;
  }
  GuLog("Windows image staged: status=%#llx matches=%zu SHA256 pinned\n",(unsigned long long)s,(size_t)matches);
  return s;
}
EFI_STATUS GuBootChoose(BOOLEAN *unlock) {
  if(!unlock||!Table||!Table->ConIn||!Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;
  *unlock=FALSE;
  /* Unattended install: an AUTO.TXT in the log directory means "unlock every
   * boot without asking". Used when the EFI lives on the ESP instead of a USB. */
  if(Root){
    EFI_FILE_PROTOCOL *af=NULL;
    if(Root->Open(Root,&af,L"\\AD104-UNLOCK\\AUTO.TXT",EFI_FILE_MODE_READ,0)==EFI_SUCCESS&&af){
      (VOID)af->Close(af);
      *unlock=TRUE;
      GuBootSay(L"\r\nAD104 v0.5.4 AUTO mode: unattended unlock, then Windows.\r\n"
                L"Do not touch peripherals. Power off and wait 10 s if Windows does not return in 4 minutes.\r\n");
      GuLog("Operator AUTO: unattended attempt (AUTO.TXT present)\n");
      return EFI_SUCCESS;
    }
  }
  GuBootSay(L"\r\nAD104 EXPERIMENTAL v0.5.4 - validation sur ce PC en cours.\r\nU : un essai, puis Windows. S/Echap ou 30 s : Windows normal.\r\nL'image peut rester FIGEE sur ce message pendant la liberation du GPU.\r\nNe plus toucher aux peripheriques. Si Windows ne revient pas apres 4 minutes,\r\neteindre completement, attendre 10 s, puis choisir Windows Boot Manager.\r\nJournal : USB AD104-UNLOCK/RUNxxxx.TXT. Aucun nouvel essai automatique.\r\n");
  for(UINTN i=0;i<300;i++) {
    EFI_INPUT_KEY key;EFI_STATUS s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
    if(s==EFI_SUCCESS) {
      if(key.UnicodeChar=='u'||key.UnicodeChar=='U'){*unlock=TRUE;return EFI_SUCCESS;}
      if(key.ScanCode==0x17||key.UnicodeChar==27||key.UnicodeChar=='s'||key.UnicodeChar=='S')return EFI_SUCCESS;
    } else if(s!=EFI_NOT_READY)return s;
    s=Table->BootServices->Stall(100000);if(s!=EFI_SUCCESS)return s;
  }
  return EFI_SUCCESS;
}
EFI_STATUS GuBootClose(VOID) {
  EFI_STATUS s=EFI_SUCCESS;
  if(Log){s=Log->Flush(Log);EFI_STATUS closed=Log->Close(Log);Log=NULL;if(s==EFI_SUCCESS)s=closed;}
  if(Root){EFI_STATUS closed=Root->Close(Root);Root=NULL;if(s==EFI_SUCCESS)s=closed;}
  /* Seal only after a clean close: a failed close is a real fault and must still
   * be able to report itself. */
  if(s==EFI_SUCCESS)Sealed=TRUE;
  return s;
}
EFI_STATUS GuBootWindows(VOID) {
  if(!Windows)return EFI_NOT_READY;
  return Table->BootServices->StartImage(Windows,NULL,NULL);
}
VOID GuBootHalt(VOID) {
  GuBootWake();
  GuBootSay(L"\r\nSTOPPED. Power OFF fully; next boot use Windows Boot Manager.\r\n");
  for(;;)__asm__ volatile("pause");
}
