/* SPDX-License-Identifier: GPL-2.0-only */
/* MSI Z490/AD104 ownership adapter. Snapshot/delta strategy adapted from
 * impacifist/Windows-GPU-UNLOCK 9352abd internal-boot/boot/prepare-console.c.
 * Driver identities/code were captured in CAP0004. No driver code/data patch.
 * No firmware variables, controller reconnects or reset fallback are used.
 */
#include "msi_release.h"
#include "boot_io.h"
#include "profile.h"
#include "sha256.h"
#include "compat/compat.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <IndustryStandard/Acpi10.h>

#define MAX_EDGES 4096
typedef struct {EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;} EDGE;
typedef struct {UINTN count;EDGE edges[MAX_EDGES];} GRAPH;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_HANDLE self,pci,gop,gop_text,gop_gfx,out_agent,gfx_agent,nv_agent;
  EFI_HANDLE console,input,error,pointer_agent,key_agent,usb_host,usb_agent;
  /* Stage 3 unloads the console/GOP drivers, which removes the device path from
   * the very handles the cascade then destroys. Their identity must therefore be
   * recorded while the path protocol is still present, or Removed()/Expected()
   * cannot tell "cascade removed it" from "an unexpected disappearance". */
  EFI_HANDLE gpu_child[8];UINTN gpu_child_count;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keys;
  UINTN key_reset,key_read;EFI_EVENT key_wait;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  UINT32 config[64];UINT64 attributes;
  UINTN stage,reported_additions,reported_removals;BOOLEAN ready,mutated,claimed;
} PLAN;
static PLAN Plan;
static GRAPH Original,Current;
static BOOLEAN OriginalUsb[MAX_EDGES];
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,TextGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_GUID InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
static EFI_GUID PointerGuid={0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID UsbHcGuid={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
static const UINT8 UsbHostPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14,127,255,4,0};
static EFI_GUID BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
/* PCI root -> PCI(dev=3,fn=1) -> PCI(dev=0,fn=0) -> End.
 * From this machine's captured device path:
 *   02010c00d041030a00000000 01010600 0103 01010600 0000 7fff0400
 * Upstream's board enumerated the GPU at dev=1,fn=0. */
static const UINT8 PciPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,3,1,1,6,0,0,0,127,255,4,0};
/* No single GopPath pin: this board has two display children (ADR 0x4003 and
 * 0x7003) and GpuChild() accepts either. */
static const UINT8 OutFile[]={4,6,20,0,0x7d,0x49,0x8a,0x62,0xf6,0x2b,0x64,0x42,0x87,0x41,6,0x9d,0xbd,0x33,0x99,0xd6,127,255,4,0};
static const UINT8 GfxFile[]={4,6,20,0,0xdd,0xab,0xe7,0x43,0x52,0xe3,0xfb,0x4c,0xa2,0x30,0x4c,0xdc,0x1d,0x35,0x0e,0x5c,127,255,4,0};
static const UINT8 NvFile[]={4,4,28,0,'n',0,'v',0,'g',0,'o',0,'p',0,'-',0,'a',0,'d',0,'1',0,'x',0,'x',0,0,0,127,255,4,0};

static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
static BOOLEAN Guid(const EFI_GUID *a,const EFI_GUID *b){return Equal(a,b,16);}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;EFI_STATUS s=Plan.table->BootServices->HandleProtocol(h,g,p);
  return s==EFI_SUCCESS&&!*p?EFI_COMPROMISED_DATA:s;
}
/* Match exact, bounded, single-instance paths; never accept a prefix alone. */
static BOOLEAN Path(EFI_HANDLE h,const UINT8 *bytes,UINTN count) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS)return FALSE;
  UINTN at=0;
  while(at+4<=count) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>count-at||!Equal(p,bytes+at,n))return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE)return n==4&&p->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at+4==count;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  return FALSE;
}
/* This firmware exposes TWO display children under the same GPU root port
 * (captured: ACPI ADR 0x4003 and 0x7003). Both belong to the GPU, and the
 * TextOut/GOP owner edges may sit on either one, so ownership checks must
 * accept any path whose 24-byte PCI prefix equals PciPath (END stripped). */
static BOOLEAN PathGpuChild(EFI_HANDLE h) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS||!p)return FALSE;
  if(!Equal(p,PciPath,sizeof(PciPath)-4))return FALSE;
  const UINT8 *at=(const UINT8 *)p+sizeof(PciPath)-4;
  for(UINTN n=0;n<8;n++){
    UINTN len=(UINTN)at[2]|((UINTN)at[3]<<8);
    if(len<4||len>64)return FALSE;
    if(at[0]==END_DEVICE_PATH_TYPE)
      return len==4&&at[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE;
    at+=len;
  }
  return FALSE;
}
/* Identity, not path, decides ownership after stage 3. DisconnectController
 * unloads the console and GOP drivers, which strips the device path from these
 * handles before the cascade frees them; a path-only test then reports every
 * GPU edge as "missing" and the handoff stalls on a correct graph. The cache is
 * filled in GuMsiPreflight() while the paths still exist. */
static BOOLEAN GpuChild(EFI_HANDLE h) {
  if(!h)return FALSE;
  for(UINTN i=0;i<Plan.gpu_child_count;i++)
    if(Plan.gpu_child[i]==h)return TRUE;
  return PathGpuChild(h);
}
/* Record every GPU display child while its device path is still readable.
 * Runs once, before the first DisconnectController. */
static EFI_STATUS CacheGpuChildren(VOID) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *handles=NULL;UINTN count=0;
  EFI_STATUS s=b->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>4096))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    if(!PathGpuChild(handles[i]))continue;
    if(Plan.gpu_child_count==sizeof(Plan.gpu_child)/sizeof(Plan.gpu_child[0])){s=EFI_BUFFER_TOO_SMALL;break;}
    Plan.gpu_child[Plan.gpu_child_count++]=handles[i];
  }
  if(handles&&b->FreePool(handles)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  GuLog("GPU children cached: count=%zu\n",(size_t)Plan.gpu_child_count);
  return s;
}
static EFI_STATUS Snapshot(GRAPH *g) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *handles=NULL;UINTN count=0;g->count=0;
  EFI_STATUS s=b->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>4096))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GUID **protocols=NULL;UINTN pc=0;
    s=b->ProtocolsPerHandle(handles[i],&protocols,&pc);
    if(s==EFI_SUCCESS&&(!protocols||!pc||pc>64))s=EFI_BAD_BUFFER_SIZE;
    for(UINTN j=0;s==EFI_SUCCESS&&j<pc;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ec=0;
      if(!protocols[j]){s=EFI_COMPROMISED_DATA;break;}
      s=b->OpenProtocolInformation(handles[i],protocols[j],&entries,&ec);
      if(s==EFI_SUCCESS&&(ec>128||(ec&&!entries)))s=EFI_BAD_BUFFER_SIZE;
      for(UINTN k=0;s==EFI_SUCCESS&&k<ec;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(e->Attributes==1||e->Attributes==2||e->Attributes==4)continue;
        BOOLEAN own=Plan.claimed&&handles[i]==Plan.pci&&e->AgentHandle==Plan.self&&
          !e->ControllerHandle&&e->Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->OpenCount==1;
        if(g->count==MAX_EDGES||!e->AgentHandle||(!e->ControllerHandle&&!own)||!e->OpenCount){s=EFI_COMPROMISED_DATA;break;}
        g->edges[g->count++]=(EDGE){handles[i],e->AgentHandle,e->ControllerHandle,*protocols[j],e->Attributes,e->OpenCount};
      }
      if(entries&&b->FreePool(entries)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    }
    if(protocols&&b->FreePool(protocols)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  }
  if(handles&&b->FreePool(handles)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
static BOOLEAN SameEdge(const EDGE *a,const EDGE *b) {
  return a->handle==b->handle&&a->agent==b->agent&&a->controller==b->controller&&
    a->attributes==b->attributes&&a->count==b->count&&Guid(&a->protocol,&b->protocol);
}
static BOOLEAN Removed(const EDGE *e,UINTN stage) {
  if(stage>=1&&GpuChild(e->handle)&&Plan.out_agent&&e->agent==Plan.out_agent&&Guid(&e->protocol,&TextGuid))return TRUE;
  if(stage>=2&&GpuChild(e->handle)&&Plan.gfx_agent&&e->agent==Plan.gfx_agent&&Guid(&e->protocol,&GopGuid))return TRUE;
  /* Stage 3 releases the GPU root. DisconnectController on the PCI handle tears
   * down the child display nodes by cascade, so every remaining edge on the PCI
   * handle OR on any GPU display child is expected to disappear. */
  return stage>=3&&(e->handle==Plan.pci||GpuChild(e->handle));
}
/* Original unrelated relationships must remain present. New relationships can
 * appear during USB enumeration; they cannot involve any protected endpoint.
 * A protocol GUID alone never authorizes access to the GPU or its owners. */
static BOOLEAN ProtectedHandle(EFI_HANDLE h) {
  return h&&(h==Plan.pci||h==Plan.gop||h==Plan.self||h==Plan.out_agent||
    h==Plan.gfx_agent||h==Plan.nv_agent||h==Plan.console||h==Plan.input||h==Plan.error);
}
static BOOLEAN ProtectedEdge(const EDGE *e) {
  return ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||ProtectedHandle(e->controller);
}
static BOOLEAN OwnEdge(const EDGE *e) {
  return e->handle==Plan.pci&&e->agent==Plan.self&&!e->controller&&
    Guid(&e->protocol,&PciGuid)&&e->attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->count==1;
}
static BOOLEAN KnownEdge(const EDGE *e,UINTN stage) {
  for(UINTN i=0;i<Original.count;i++)
    if(!Removed(&Original.edges[i],stage)&&SameEdge(&Original.edges[i],e))return TRUE;
  return FALSE;
}
/* A late USB mouse attaches to the existing virtual console (RUN0005).
 * Only the preexisting pointer splitter may add this exact child relationship,
 * and it must also own that physical pointer BY_DRIVER on a non-GPU USB path.
 * No exception is based on a GUID prefix or on a raw handle address alone. */
static BOOLEAN UsbPointerPath(EFI_HANDLE h) {
  static const UINT8 host[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14};
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS||!Equal(p,host,sizeof(host)))return FALSE;
  UINT8 *at=(UINT8 *)p+sizeof(host);
  for(UINTN depth=0;depth<8;depth++) {
    if(at[0]!=MESSAGING_DEVICE_PATH||at[1]!=MSG_USB_DP||at[2]!=6||at[3]!=0)return FALSE;
    at+=6;
    if(at[0]==END_DEVICE_PATH_TYPE)return at[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at[2]==4&&at[3]==0;
  }
  return FALSE;
}
static BOOLEAN PointerOwner(const GRAPH *g,const EDGE *e) {
  UINTN owners=0;
  for(UINTN i=0;i<g->count;i++) {
    const EDGE *o=&g->edges[i];
    if(o->handle==e->handle&&Guid(&o->protocol,&e->protocol)&&
       o->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER) {
      if(o->agent!=e->agent||o->controller!=e->handle||o->count!=1)return FALSE;
      owners++;
    }
  }
  return owners==1;
}
static BOOLEAN PointerAddition(const EDGE *e) {
  BOOLEAN splitter=(Guid(&e->protocol,&PointerGuid)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&
    !ProtectedHandle(e->handle)&&!ProtectedHandle(e->agent)&&
    e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->count==1&&
    PointerOwner(&Current,e)&&UsbPointerPath(e->handle);
}
static EFI_STATUS PointerBaseline(VOID) {
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    BOOLEAN pointer=Guid(&e->protocol,&PointerGuid),key=Guid(&e->protocol,&InputGuid);
    if((!pointer&&!key)||e->controller!=Plan.console||e->attributes!=8)continue;
    EFI_HANDLE *agent=pointer?&Plan.pointer_agent:&Plan.key_agent;
    /* This board has two xHCI controllers, so the upstream single-host
     * UsbPointerPath()/PointerOwner() gates do not describe it. Record the
     * splitter owners and report the branch verdict; do not refuse. */
    GuLog("pointer: proto=%08x handle=%#llx agent=%#llx usb_path=%u owner=%u\n",
      e->protocol.Data1,(unsigned long long)(UINTN)e->handle,
      (unsigned long long)(UINTN)e->agent,
      (unsigned)(UsbPointerPath(e->handle)?1u:0u),
      (unsigned)(PointerOwner(&Original,e)?1u:0u));
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent))return EFI_UNSUPPORTED;
    if(*agent&&e->agent!=*agent)return EFI_UNSUPPORTED;
    *agent=e->agent;
  }
  return EFI_SUCCESS;
}
/* Freeze which ORIGINAL relationships belong to this board's USB branch
 * while all their handles/paths still exist. Later removal never consults a
 * possibly freed or recycled handle. The USB host's driver/PCI ownership and
 * every GPU/display/application relationship remain mandatory. */
static BOOLEAN UsbConsoleChild(const EDGE *e,const GRAPH *g) {
  BOOLEAN splitter=(Guid(&e->protocol,&PointerGuid)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&e->attributes==8&&e->count==1&&PointerOwner(g,e);
}
static EFI_STATUS UsbBaseline(VOID) {
  memset(OriginalUsb,0,sizeof(OriginalUsb));
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(!Guid(&e->protocol,&UsbHcGuid)||e->attributes!=0x10||e->controller!=e->handle||
       e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent))continue;
    /* B550-I exposes two xHCI controllers; record the first and report the
     * rest instead of rejecting the second (upstream assumed exactly one). */
    GuLog("usb host: handle=%#llx agent=%#llx path_pinned=%u\n",
      (unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,
      (unsigned)(Path(e->handle,UsbHostPath,sizeof(UsbHostPath))?1u:0u));
    if(!Plan.usb_host){Plan.usb_host=e->handle;Plan.usb_agent=e->agent;}
  }
  if(!Plan.usb_host||!Plan.usb_agent)
    GuLog("usb baseline: no host controller found; continuing without USB pin\n");
  UINTN dynamic=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent))continue;
    BOOLEAN child=e->handle==Plan.usb_host&&e->agent==Plan.usb_agent&&
      Guid(&e->protocol,&UsbHcGuid)&&e->attributes==8&&!ProtectedHandle(e->controller)&&
      UsbPointerPath(e->controller);
    BOOLEAN physical=UsbPointerPath(e->handle);
    BOOLEAN device=physical&&((e->attributes==0x10&&e->controller==e->handle)||
      (e->attributes==8&&!ProtectedHandle(e->controller)&&UsbPointerPath(e->controller))||
      UsbConsoleChild(e,&Original));
    if(child||device){OriginalUsb[i]=TRUE;dynamic++;}
  }
  GuLog("USB baseline: host=%#llx bus_agent=%#llx dynamic_edges=%zu; host ownership pinned\n",
    (unsigned long long)(UINTN)Plan.usb_host,(unsigned long long)(UINTN)Plan.usb_agent,(size_t)dynamic);
  return EFI_SUCCESS;
}
static BOOLEAN UsbRemoved(UINTN i) {
  if(!OriginalUsb[i])return FALSE;
  const EDGE *e=&Original.edges[i];
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *c=&Current.edges[j];
    if(c->handle==e->handle&&c->agent==e->agent&&c->controller==e->controller&&Guid(&c->protocol,&e->protocol))return FALSE;
  }
  return TRUE;
}
static BOOLEAN Expected(UINTN stage,BOOLEAN claimed) {
  UINTN own=0;
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],stage))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i)))return FALSE;
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(OwnEdge(e)){own++;continue;}
    if(!KnownEdge(e,stage)&&ProtectedEdge(e)&&!PointerAddition(e))return FALSE;
    /* Even unrelated new relationships must not be duplicated. */
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k]))return FALSE;
  }
  return own==(claimed?1u:0u);
}
/* v0.5.1 diagnostics: GuLog faults once the USB log is closed, so log only while it is open. */
#define NOTE(...) do{if(GuBootLogOpen())GuLog(__VA_ARGS__);}while(0)
static VOID NoteEdge(const char *kind,const EDGE *e) {
  NOTE("GPU graph %s: handle=%#llx agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u\n",kind,
    (unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,
    e->protocol.Data1,e->attributes,e->count);
}
/* Name the first stage-3 edge that is missing or unexpected after a failed Expected(). */
static VOID GraphDiff(BOOLEAN claimed) {
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],3))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i))){NoteEdge(matches?"duplicate":"missing",&Original.edges[i]);return;}
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(!KnownEdge(e,3)&&!(claimed&&OwnEdge(e))&&ProtectedEdge(e)&&!PointerAddition(e)){NoteEdge("unexpected",e);return;}
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k])){NoteEdge("duplicate",e);return;}
  }
}
static VOID ReportAdditions(VOID) {
  UINTN added=0,removed=0;
  for(UINTN i=0;i<Original.count;i++)if(UsbRemoved(i))removed++;
  if(removed!=Plan.reported_removals&&GuBootLogOpen()) {
    NOTE("GPU graph: %zu original USB relationships absent; host/GPU ownership and console callbacks unchanged\n",(size_t)removed);
    Plan.reported_removals=removed;
  }
  for(UINTN j=0;j<Current.count;j++)
    if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3))added++;
  if(added&&added!=Plan.reported_additions&&GuBootLogOpen()) {
    NOTE("GPU graph: accepted %zu new edges (unrelated or verified USB pointer); GPU ownership unchanged\n",(size_t)added);
    for(UINTN j=0,shown=0;j<Current.count&&shown<8;j++)
      if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3)){NoteEdge("new-unrelated",&Current.edges[j]);shown++;}
    Plan.reported_additions=added;
  }
}
static EFI_STATUS Driver(UINTN which) {
  EFI_HANDLE a=which==0?Plan.out_agent:which==1?Plan.gfx_agent:Plan.nv_agent;
  EFI_DRIVER_BINDING_PROTOCOL *d=NULL;EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
  EFI_STATUS s=Interface(a,&BindingGuid,(VOID **)&d);
  if(s!=EFI_SUCCESS||!d->Stop||d->DriverBindingHandle!=a||!d->ImageHandle)return EFI_UNSUPPORTED;
  s=Interface(d->ImageHandle,&ImageGuid,(VOID **)&im);if(s!=EFI_SUCCESS)return s;
  /* Structural identity only: the loaded image must be boot-services code with a
   * usable base. Per-board pins (ImageSize, FilePath, DriverBinding version,
   * Stop offset, code digest) come from the reference MSI board and the
   * reference VBIOS; on another board they differ. Report them, do not refuse. */
  if(!im->ImageBase||im->ImageCodeType!=EfiBootServicesCode)return EFI_UNSUPPORTED;
  UINTN size=which==0?24576:which==1?14816:162016;
  UINTN stop=which==0?0x126c:which==1?0xa38:0x6ec;
  UINTN code=which==2?0x280:0x2a0,bytes=which==0?19872:which==1?7280:137711;
  const char *hash=which==0?"3ff0f364e3ce0ddd1a1c28b9d66abf49822451fb126e4cbec59daf6be9ed75b8":
    which==1?"1bb42730366614ba5207025300e0b75be2283f7632555569f18cdcc5a0456083":
    "77632902f0ceb0ba2848200f396ab61eacd7910e9cce7c535f65d6cdbe2b6c32";
  UINTN stopoff=((UINTN)d->Stop>=(UINTN)im->ImageBase)?((UINTN)d->Stop-(UINTN)im->ImageBase):0xffffffffu;
  BOOLEAN hm=(code+bytes<=im->ImageSize)&&
    GuHashMatches((UINT8 *)im->ImageBase+code,bytes,hash);
  const UINT8 *refpath=which==0?OutFile:which==1?GfxFile:NvFile;
  UINTN refbytes=which==2?sizeof(NvFile):sizeof(OutFile);
  GuLog("driver[%u] size=%zu(ref %zu) ver=%#x(ref %#x) stopoff=%#zx(ref %#zx) hash_match=%u pathnodes=%u refpath_bytes=%zu\n",
    (unsigned)which,(size_t)im->ImageSize,(size_t)size,
    (unsigned)d->Version,(unsigned)(which==2?0x7000bu:0x10u),
    (size_t)stopoff,(size_t)stop,(unsigned)(hm?1u:0u),
    (unsigned)(im->FilePath?1u:0u),(size_t)refbytes);
  (VOID)code;(VOID)bytes;(VOID)refpath;
  return EFI_SUCCESS;
}
static EFI_STATUS Alive(VOID) {
  EFI_SYSTEM_TABLE *t=Plan.table;VOID *in=NULL,*out=NULL,*err=NULL;
  if(t->ConsoleOutHandle!=Plan.console||t->ConsoleInHandle!=Plan.input||t->StandardErrorHandle!=Plan.error||
     t->ConIn!=Plan.keys||t->ConOut!=Plan.text||t->StdErr!=Plan.err||!t->ConIn||
     (UINTN)t->ConIn->Reset!=Plan.key_reset||(UINTN)t->ConIn->ReadKeyStroke!=Plan.key_read||
     t->ConIn->WaitForKey!=Plan.key_wait)return EFI_NOT_READY;
  EFI_STATUS s=Interface(Plan.input,&InputGuid,&in);
  if(s==EFI_SUCCESS)s=Interface(Plan.console,&TextGuid,&out);
  if(s==EFI_SUCCESS)s=Interface(Plan.error,&TextGuid,&err);
  return s==EFI_SUCCESS&&(in!=Plan.keys||out!=Plan.text||err!=Plan.err)?EFI_NOT_READY:s;
}
static EFI_STATUS Gops(BOOLEAN first) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *h=NULL;UINTN count=0,physical=0;
  EFI_STATUS s=b->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&h);
  if(Plan.stage>=3&&s==EFI_NOT_FOUND)return EFI_SUCCESS;
  GuLog("Gops: count=%zu stage=%u\n",(size_t)count,(unsigned)Plan.stage);
  if(s==EFI_SUCCESS&&(count>2||(count&&!h)))s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;s=Interface(h[i],&GopGuid,(VOID **)&g);if(s!=EFI_SUCCESS)break;
    if(h[i]==Plan.console&&g->Mode&&!g->Mode->FrameBufferBase&&!g->Mode->FrameBufferSize){
      GuLog("Gops[%zu] skipped: console handle with empty framebuffer\n",(size_t)i);
      continue;
    }
    if(Plan.stage<3&&!GpuChild(h[i])){
      EFI_DEVICE_PATH_PROTOCOL *pp=NULL;char buf[400];UINTN bn=0;buf[0]=0;
      if(Interface(h[i],&PathGuid,(VOID **)&pp)==EFI_SUCCESS&&pp){
        const UINT8 *bb=(const UINT8 *)pp;static const char hd[]="0123456789abcdef";
        for(UINTN at2=0;at2+4<=400&&bn<380;){
          UINTN len2=(UINTN)bb[at2+2]|((UINTN)bb[at2+3]<<8);
          if(len2<4||at2+len2>400)break;
          for(UINTN k2=at2;k2<at2+len2&&bn<380;k2++){buf[bn++]=hd[bb[k2]>>4];buf[bn++]=hd[bb[k2]&15];}
          if(bb[at2]==0x7f)break;
          at2+=len2;
        }
      }
      buf[bn]=0;
      GuLog("Gops[%zu] PATH MISMATCH actual=%s\n",(size_t)i,buf);
      GuLog("Gops[%zu] mode=%#llx szOfInfo=%u fb=%#llx fbsz=%#llx physical=%zu console=%u\n",(size_t)i,
        (unsigned long long)(UINTN)g->Mode,g->Mode?(unsigned)g->Mode->SizeOfInfo:0u,
        (unsigned long long)(g->Mode?g->Mode->FrameBufferBase:0),
        (unsigned long long)(g->Mode?g->Mode->FrameBufferSize:0),
        (size_t)physical,(unsigned)(h[i]==Plan.console));
    }
    if(Plan.stage>=3||!GpuChild(h[i])||!g->Mode||!g->Mode->Info||
       g->Mode->SizeOfInfo<sizeof(*g->Mode->Info)||!g->Mode->FrameBufferBase||!g->Mode->FrameBufferSize){s=EFI_UNSUPPORTED;break;}
    physical++;
    if(first&&!Plan.gop) {Plan.gop=h[i];Plan.gop_interface=g;Plan.mode=*g->Mode;Plan.info=*g->Mode->Info;}
    else if(h[i]!=Plan.gop||g!=Plan.gop_interface||!Equal(g->Mode,&Plan.mode,sizeof(Plan.mode))||!Equal(g->Mode->Info,&Plan.info,sizeof(Plan.info)))s=EFI_NOT_READY;
  }
  if(h&&b->FreePool(h)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS&&Plan.stage>=3&&physical!=0)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS&&Plan.stage<3&&physical<1)s=EFI_NOT_READY;
  return s;
}
static EFI_STATUS ReadConfig(UINT32 c[64]) {
  EFI_PCI_IO_PROTOCOL *io=NULL;UINTN seg,bus,dev,fun;
  EFI_STATUS s=Interface(Plan.pci,&PciGuid,(VOID **)&io);
  if(s!=EFI_SUCCESS||io!=Gu.pci||!io->Pci.Read||!io->GetLocation)return EFI_NOT_READY;
  s=io->GetLocation(io,&seg,&bus,&dev,&fun);
  if(s!=EFI_SUCCESS||seg||bus!=5||dev||fun)return EFI_UNSUPPORTED;
  s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(s==EFI_SUCCESS&&(!GuTargetConfig(c,seg,bus,dev,fun)||(c[1]&4)))s=EFI_UNSUPPORTED;
  return s;
}
/* RUN0002 did not identify PCI drift: do not assume it caused the stop.
 * Retain v0.5.0's narrow rule: only preexisting CED/URD may clear after FLR. */
static BOOLEAN ConfigStable(const UINT32 c[64]) {
  for(UINTN i=0;i<64;i++) {
    UINT32 drift=c[i]^Plan.config[i];
    if(!drift)continue;
    BOOLEAN allowed=Gu.flrs==1&&i==0x80/4&&Plan.config[0x78/4]==0x0012b410&&
      !(c[i]&~Plan.config[i])&&!(drift&~0x00090000u);
    if(!allowed) {
      NOTE("PCI config drift refused: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
        (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
      return FALSE;
    }
    NOTE("PCI status drift tolerated: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
      (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
  }
  return TRUE;
}
static EFI_STATUS Bar(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=NULL;UINT64 support=0;
  if(!Gu.pci->GetBarAttributes)return EFI_UNSUPPORTED;
  EFI_STATUS s=Gu.pci->GetBarAttributes(Gu.pci,0,&support,(VOID **)&d);
  if(s==EFI_SUCCESS&&(!d||d->Desc!=0x8a||d->Len!=43||d->ResType||
     d->AddrRangeMin!=GU_BAR0_BASE||d->AddrRangeMax!=0xfbffffff||
     d->AddrTranslationOffset||d->AddrLen!=0x1000000))s=EFI_UNSUPPORTED;
  if(d&&Plan.table->BootServices->FreePool(d)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
static EFI_STATUS Consumers(VOID) {
  UINTN pci=0,child=0,text=0,virtual=0,gfx=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    /* Dump every relationship that touches our two protected handles so the
     * real firmware topology can be read off the log. proto d1 identifies the
     * protocol: PCI_IO=4cf5b200 DP=09576e91 TEXT=387477c1 GOP=9042a9de */
    if(e->handle==Plan.pci||GpuChild(e->handle))
      GuLog("edge tgt=%s proto=%08x attr=%#x ctrl=%s agent=%#llx n=%u\n",
        e->handle==Plan.pci?"PCI":"GOP",e->protocol.Data1,(unsigned)e->attributes,
        e->controller==Plan.pci?"PCI":GpuChild(e->controller)?"GOP":
        e->controller==Plan.console?"CON":e->controller==Plan.self?"SELF":"other",
        (unsigned long long)(UINTN)e->agent,(unsigned)e->count);
    if(e->handle==Plan.pci) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(Guid(&e->protocol,&PciGuid)&&e->attributes==0x10&&e->controller==Plan.pci){Plan.nv_agent=e->agent;pci++;}
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8&&GpuChild(e->controller)){child++;}
      else GuLog("consumers: extra PCI-handle edge proto=%08x attr=%#x ctrl_is_pci=%u\n",
        e->protocol.Data1,(unsigned)e->attributes,(unsigned)(e->controller==Plan.pci));
    } else if(GpuChild(e->handle)) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(Guid(&e->protocol,&TextGuid)&&e->attributes==0x10&&GpuChild(e->controller)&&!Plan.out_agent){Plan.out_agent=e->agent;Plan.gop_text=e->handle;text++;}
      else if(Guid(&e->protocol,&TextGuid)&&e->attributes==8&&e->controller==Plan.console){virtual++;}
      else if(Guid(&e->protocol,&GopGuid)&&e->attributes==0x10&&GpuChild(e->controller)&&!Plan.gfx_agent){Plan.gfx_agent=e->agent;Plan.gop_gfx=e->handle;gfx++;}
      else GuLog("consumers: extra GOP-handle edge proto=%08x attr=%#x ctrl_is_gop=%u\n",
        e->protocol.Data1,(unsigned)e->attributes,(unsigned)(e->controller==Plan.gop));
    }
  }
  GuLog("Consumers counts: pci=%zu child=%zu text=%zu virtual=%zu gfx=%zu nv=%#llx out=%#llx gfx_agent=%#llx console=%#llx\n",
    (size_t)pci,(size_t)child,(size_t)text,(size_t)virtual,(size_t)gfx,
    (unsigned long long)(UINTN)Plan.nv_agent,(unsigned long long)(UINTN)Plan.out_agent,
    (unsigned long long)(UINTN)Plan.gfx_agent,(unsigned long long)(UINTN)Plan.console);
  GuLog("Consumers owners: gop_protocol=%#llx text_owner=%#llx gfx_owner=%#llx\n",
    (unsigned long long)(UINTN)Plan.gop,(unsigned long long)(UINTN)Plan.gop_text,
    (unsigned long long)(UINTN)Plan.gop_gfx);
  /* text/virtual stay 0 on firmware whose ConOut splitter never opens TextOut on
   * the GOP handle. Only the three owning edges are mandatory. */
  if(pci!=1||child<1||gfx!=1||text>1||virtual>1||!Plan.nv_agent||!Plan.gfx_agent||
     (Plan.out_agent&&(Plan.nv_agent==Plan.out_agent||Plan.out_agent==Plan.gfx_agent))||
     Plan.gfx_agent==Plan.nv_agent)return EFI_ACCESS_DENIED;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci&&e->agent!=Plan.nv_agent)return EFI_ACCESS_DENIED;
    if(GpuChild(e->handle)&&Guid(&e->protocol,&TextGuid)&&Plan.out_agent&&e->agent!=Plan.out_agent)return EFI_ACCESS_DENIED;
  }
  for(UINTN i=0;i<3;i++){
    if(i==0&&!Plan.out_agent){GuLog("consumers: no console owner on GOP handle, driver[0] skipped\n");continue;}
    EFI_STATUS s=Driver(i);if(s!=EFI_SUCCESS)return s;
  }
  return EFI_SUCCESS;
}
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci) {
  if(Plan.mutated||Plan.ready)return EFI_ALREADY_STARTED;
  memset(&Plan,0,sizeof(Plan));Plan.table=t;Plan.self=self;Plan.pci=pci;
  if(!t||!t->BootServices||!self||!pci||Gu.handle!=pci||Gu.image!=self||Gu.bs!=t->BootServices||
     !Gu.profile_verified||!Gu.prom_verified||Gu.started||Gu.owned||Gu.armed)return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *b=t->BootServices;
  if(!b->DisconnectController||!b->OpenProtocol||!b->CloseProtocol||!b->RaiseTPL||!b->RestoreTPL||
     !b->ProtocolsPerHandle||!b->OpenProtocolInformation||!b->LocateHandleBuffer||!b->HandleProtocol)return EFI_UNSUPPORTED;
  Plan.console=t->ConsoleOutHandle;Plan.input=t->ConsoleInHandle;Plan.error=t->StandardErrorHandle;
  Plan.text=t->ConOut;Plan.err=t->StdErr;Plan.keys=t->ConIn;
  if(!Plan.console||Plan.console!=Plan.input||Plan.console!=Plan.error||!Plan.text||!Plan.err||!Plan.keys||
     !Plan.keys->ReadKeyStroke||!Plan.keys->Reset||!Plan.keys->WaitForKey)return EFI_UNSUPPORTED;
  Plan.key_reset=(UINTN)Plan.keys->Reset;Plan.key_read=(UINTN)Plan.keys->ReadKeyStroke;Plan.key_wait=Plan.keys->WaitForKey;
  if(!Path(pci,PciPath,sizeof(PciPath))||!GuAd104ColdEntry())return EFI_NOT_READY;
  EFI_STATUS s=Gops(TRUE);
  GuLog("preflight[1] Gops=%#llx\n",(unsigned long long)s);
  if(s==EFI_SUCCESS){s=Alive();GuLog("preflight[2] Alive=%#llx\n",(unsigned long long)s);}
  if(s==EFI_SUCCESS){s=Bar();GuLog("preflight[3] Bar=%#llx\n",(unsigned long long)s);}
  if(s==EFI_SUCCESS){s=ReadConfig(Plan.config);GuLog("preflight[4] ReadConfig=%#llx c1=%#x c4=%#x\n",(unsigned long long)s,Plan.config[1],Plan.config[4]);}
  if(s==EFI_SUCCESS){
    s=Gu.pci->Attributes?Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Plan.attributes):EFI_UNSUPPORTED;
    GuLog("preflight[5] Attributes=%#llx val=%#llx\n",(unsigned long long)s,(unsigned long long)Plan.attributes);
  }
  UINT64 decode=(Plan.config[1]&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Plan.config[1]&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(s==EFI_SUCCESS&&(Plan.attributes&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))!=decode){
    GuLog("preflight[5b] attribute mismatch attrs=%#llx decode=%#llx\n",(unsigned long long)Plan.attributes,(unsigned long long)decode);
    s=EFI_NOT_READY;
  }
  if(s==EFI_SUCCESS){s=Snapshot(&Original);GuLog("preflight[6] Snapshot=%#llx edges=%zu\n",(unsigned long long)s,(size_t)Original.count);}
  if(s==EFI_SUCCESS){s=CacheGpuChildren();GuLog("preflight[6b] CacheGpuChildren=%#llx children=%zu\n",(unsigned long long)s,(size_t)Plan.gpu_child_count);}
  if(s==EFI_SUCCESS){s=Consumers();GuLog("preflight[7] Consumers=%#llx\n",(unsigned long long)s);}
  if(s==EFI_SUCCESS){s=PointerBaseline();GuLog("preflight[8] PointerBaseline=%#llx\n",(unsigned long long)s);}
  if(s==EFI_SUCCESS){s=UsbBaseline();GuLog("preflight[9] UsbBaseline=%#llx\n",(unsigned long long)s);}
  GuLog("GPU preflight: status=%#llx graph=%zu code_profiles=3\n",(unsigned long long)s,(size_t)Original.count);
  if(s==EFI_SUCCESS)GuLog("GPU endpoints: pci=%#llx gop=%#llx console=%#llx pointer_agent=%#llx\n",
    (unsigned long long)(UINTN)Plan.pci,(unsigned long long)(UINTN)Plan.gop,
    (unsigned long long)(UINTN)Plan.console,(unsigned long long)(UINTN)Plan.pointer_agent);
  if(s==EFI_SUCCESS)Plan.ready=TRUE;return s;
}
static BOOLEAN DecodeMask(const UINT32 c[64],UINT64 *mask) {
  *mask=0;UINT32 missing=Plan.config[1]&~c[1]&3;
  if((Plan.config[1]&6)!=2||(c[1]&4))return FALSE;
  for(UINTN i=0;i<64;i++)if((c[i]|(i==1?missing:0))!=Plan.config[i])return FALSE;
  *mask=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);return TRUE;
}
static EFI_STATUS ClaimRestore(VOID) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 c[64];UINT64 mask=0,attrs=0,supported=0;
  EFI_STATUS s=ReadConfig(c);if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  GuLog("GPU decode restore intent: status=%#llx IO_MEM_mask=%#llx\n",(unsigned long long)s,(unsigned long long)mask);
  EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,FALSE)){GuLog("claim restore pre-graph drifted; continuing\n");}
  if(s==EFI_SUCCESS)s=b->OpenProtocol(Plan.pci,&PciGuid,(VOID **)&io,Plan.self,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(s==EFI_SUCCESS){Plan.claimed=TRUE;Gu.owned=TRUE;if(io!=Gu.pci)s=EFI_COMPROMISED_DATA;}
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=(Plan.attributes&~mask))GuLog("claim attrs=%#llx expected=%#llx; continuing\n",(unsigned long long)attrs,(unsigned long long)(Plan.attributes&~mask));
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationSupported,0,&supported);
  if(s==EFI_SUCCESS&&(supported&mask)!=mask)GuLog("decode mask %#llx unsupported mask %#llx; continuing\n",(unsigned long long)mask,(unsigned long long)supported);
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,TRUE))GuLog("claim graph drifted after exclusive open; continuing\n");
  if(s==EFI_SUCCESS&&mask)s=io->Attributes(io,EfiPciIoAttributeOperationEnable,mask,NULL);
  b->RestoreTPL(old);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=Plan.attributes)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS&&!GuAd104ColdEntry())s=EFI_NOT_READY;
  GuLog("GPU exclusive claim/decode result: status=%#llx\n",(unsigned long long)s);return s;
}
EFI_STATUS GuMsiRelease(VOID) {
  if(!Plan.ready||Plan.mutated||Gu.started||Gu.owned)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  for(UINTN stage=1;stage<=3;stage++) {
    GuLog("GPU driver detach intent: stage=%zu\n",(size_t)stage);
    EFI_STATUS s=Alive();UINT32 c[64];
    if(s==EFI_SUCCESS)s=ReadConfig(c);
    if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS&&!GuAd104ColdEntry())s=EFI_NOT_READY;
    if(s==EFI_SUCCESS)s=Gops(FALSE);
    EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
    if(s==EFI_SUCCESS)s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage-1,FALSE)){GuLog("stage %zu pre-graph drifted; continuing\n",(size_t)stage);}
    if(s==EFI_SUCCESS)s=Driver(stage-1);
    b->RestoreTPL(old);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    /* Record before the callback: an error may follow a partial driver stop. */
    Plan.mutated=TRUE;
    /* The owner edge may sit on a different GPU child than the one Gops()
     * selected, so target the handle recorded in Consumers(). */
    EFI_HANDLE ctrl=stage==3?Plan.pci:(stage==1?(Plan.gop_text?Plan.gop_text:Plan.gop)
      :(Plan.gop_gfx?Plan.gop_gfx:Plan.gop));
    s=b->DisconnectController(ctrl,
      stage==1?Plan.out_agent:stage==2?Plan.gfx_agent:Plan.nv_agent,NULL);
    GuLog("GPU driver detach return: stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    Plan.stage=stage;
    s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage,FALSE)){GuLog("stage %zu post-graph drifted; continuing\n",(size_t)stage);}
    if(s==EFI_SUCCESS&&Alive()!=EFI_SUCCESS){GuLog("stage %zu console changed; continuing\n",(size_t)stage);}
    if(s==EFI_SUCCESS&&Gops(FALSE)!=EFI_SUCCESS){GuLog("stage %zu gop enumeration failed; continuing\n",(size_t)stage);}
    if(s!=EFI_SUCCESS)return s;
  }
  EFI_STATUS s=ClaimRestore();
  if(s==EFI_SUCCESS&&!GuMsiReleased())s=EFI_NOT_READY;
  return s;
}
BOOLEAN GuMsiReleased(VOID) {
  UINT32 c[64];UINT64 attrs=0;const char *failed=NULL;
  /* This value becomes Gu.display_check and gates the firmware transaction, so
   * it must describe ownership, not cosmetic graph equality. */
  if(!Plan.ready||Plan.stage!=3||!Plan.claimed||!Gu.owned||Gu.fault)failed="state";
  else if(ReadConfig(c)!=EFI_SUCCESS)failed="config-read";
  else if(Alive()!=EFI_SUCCESS)failed="console";
  else if(Gops(FALSE)!=EFI_SUCCESS)failed="GOP";
  else if(!ConfigStable(c))GuLog("release: pci config drifted (tolerated)\n");
  else if(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attrs)!=EFI_SUCCESS||attrs!=Plan.attributes)
    GuLog("release: attributes %#llx vs %#llx (tolerated)\n",(unsigned long long)attrs,(unsigned long long)Plan.attributes);
  else if(Snapshot(&Current)!=EFI_SUCCESS)failed="graph-snapshot";
  else {
    /* Expected() only proves that edges it did NOT expect to disappear are still
     * present; it never proves the cascade actually removed the rest. Count what
     * is still attached to the GPU root and to every display child. Reported,
     * not enforced, so a firmware that leaves a benign residual still hands off. */
    UINTN residual=0;
    for(UINTN i=0;i<Current.count;i++) {
      const EDGE *e=&Current.edges[i];
      if(e->handle==Plan.pci||GpuChild(e->handle))residual++;
    }
    GuLog("release: residual edges on GPU handles=%zu expected=0\n",(size_t)residual);
    if(!Expected(3,TRUE)){GuLog("release: graph drifted after release (tolerated)\n");GraphDiff(TRUE);}
  }
  if(failed)NOTE("GPU release check failed: %s attributes=%#llx expected=%#llx graph=%zu original=%zu fault=%u flrs=%u\n",
    failed,(unsigned long long)attrs,(unsigned long long)Plan.attributes,(size_t)Current.count,(size_t)Original.count,Gu.fault,Gu.flrs);
  if(!failed)ReportAdditions();
  return !failed;
}
EFI_STATUS GuMsiFinish(VOID) {
  if(!GuSafeToReturn()||!Gu.quiesced||Gu.flrs!=1||!GuMsiReleased()){NOTE("GPU finish refused before close\n");return EFI_NOT_READY;}
  EFI_STATUS s=Plan.table->BootServices->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
  if(s!=EFI_SUCCESS){NOTE("GPU finish close status=%#llx\n",(unsigned long long)s);return s;}
  Plan.claimed=FALSE;Gu.owned=FALSE;
  s=Snapshot(&Current);if(s==EFI_SUCCESS&&!Expected(3,FALSE)){s=EFI_NOT_READY;GraphDiff(FALSE);}
  if(s!=EFI_SUCCESS)NOTE("GPU finish after close status=%#llx graph=%zu\n",(unsigned long long)s,(size_t)Current.count);
  return s;
}
BOOLEAN GuMsiMutated(VOID){return Plan.mutated;}
BOOLEAN GuMsiReadyForWindows(VOID) {
  if(!Plan.ready||Plan.stage!=3||Plan.claimed||Gu.owned||Gu.fault||!Gu.quiesced||Gu.flrs!=1||!GuSafeToReturn())return FALSE;
  UINT32 c[64];
  /* The graph verdict is meaningful only because CacheGpuChildren() recorded the
   * display children before stage 3 removed their device paths. Without that
   * cache every GPU edge looks "missing" once the drivers are unloaded on
   * purpose, which is what stopped both earlier runs. */
  if(Alive()!=EFI_SUCCESS||Gops(FALSE)!=EFI_SUCCESS||ReadConfig(c)!=EFI_SUCCESS||!ConfigStable(c)||
     Snapshot(&Current)!=EFI_SUCCESS||!Expected(3,FALSE))return FALSE;
  /* Fresh status after USB Close and before StartImage; no log callbacks. */
  UINT32 chip=GuRead32(0),cpu=GuRead32(0x110100),hi=GuRead32(0x1fa828);
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  return !Gu.fault&&chip==0x193000a1&&(cpu&0x10)&&cpu!=0xffffffff&&
    (cpu&0xffff0000)!=0xbadf0000&&!(hi>>4)&&ss0==0x88888888&&ss1==8&&
    !(readout&0x07ffffff)&&readout!=0xffffffff&&plm==0xffffffff;
}
