/* SPDX-License-Identifier: GPL-2.0-only */
#include "port.h"
#include "checks.h"
#include "profile.h"
#include "msi_release.h"
#include "boot_io.h"
#include "compat/compat.h"
#include "vendor/unlock_610.h"
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static EFI_STATUS Discover(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  EFI_HANDLE *h=NULL,target=NULL;EFI_PCI_IO_PROTOCOL *io=NULL;UINTN count=0,found=0;
  EFI_STATUS s=t->BootServices->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&h);
  if(s==EFI_SUCCESS&&(!h||!count||count>1024))s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_PCI_IO_PROTOCOL *p=NULL;UINT32 id=0;
    s=t->BootServices->HandleProtocol(h[i],&PciGuid,(VOID **)&p);
    if(s!=EFI_SUCCESS||!p||!p->Pci.Read){s=EFI_UNSUPPORTED;break;}
    s=p->Pci.Read(p,EfiPciIoWidthUint32,0,1,&id);
    if(s==EFI_SUCCESS&&id==GU_BOARD_ID){found++;target=h[i];io=p;}
  }
  if(h)t->BootServices->FreePool(h);
  if(s!=EFI_SUCCESS||found!=1||!io)return EFI_UNSUPPORTED;
  GuInit(t->BootServices,io);Gu.image=image;Gu.handle=target;
  Gu.bar_base=GU_BAR0_BASE;Gu.bar_size=0x1000000;
  UINT32 c[64];UINTN seg,bus,dev,fun;GU_CAPS caps;
  if(!io->GetLocation)return EFI_UNSUPPORTED;
  s=io->GetLocation(io,&seg,&bus,&dev,&fun);
  if(s==EFI_SUCCESS)s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(s!=EFI_SUCCESS||!GuCheckConfig(c,seg,bus,dev,fun,&caps)||c[2]!=0x030000a1)return EFI_UNSUPPORTED;
  Gu.pm_cap=caps.pm;Gu.pcie_cap=caps.pcie;Gu.pcie_version=caps.pcie_version;Gu.msi_cap=caps.msi;Gu.msix_cap=caps.msix;
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  if(!image||!t||!t->BootServices||!t->RuntimeServices||!t->RuntimeServices->GetVariable)return EFI_INVALID_PARAMETER;
  UINT8 secure=1;UINTN n=1;UINT32 attrs=0;
  EFI_STATUS s=t->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,&attrs,&n,&secure);
  if(s!=EFI_SUCCESS||n!=1||secure)return EFI_SECURITY_VIOLATION;
  s=GuBootOpen(image,t);if(s!=EFI_SUCCESS)return s;
  const char *stage="discover";BOOLEAN unlock=FALSE;
  s=Discover(image,t);if(s!=EFI_SUCCESS)goto failed;
  GuLog("AD104 MSI EXPERIMENTAL v0.5.4: hardware unvalidated; USB manual one-shot; bounded USB enumeration additions/removals; GPU ownership pinned\n");
  stage="assets";s=GuBootLoadAssets();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="read-only-cold-profile";s=GuAd104Prepare();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="stage-windows-image";s=GuBootLoadWindows();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="manual-choice";s=GuBootChoose(&unlock);if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  if(!unlock) {
    GuLog("Operator skipped: no GPU transaction, no driver release; stock Windows\n");
    if(Gu.fault||GuBootClose()!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
    return GuBootWindows();
  }
  stage="MSI-preflight";s=GuMsiPreflight(t,image,Gu.handle);if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  GuBootSay(L"\r\nChecks passed. Releasing display; watch for Windows.\r\n");
  GuLog("Operator U: one physical attempt; no retries; display callbacks disabled\n");
  if(Gu.fault){s=EFI_DEVICE_ERROR;goto failed;}
  GuBootSilence();
  stage="MSI-release";s=GuMsiRelease();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  if(!GuMsiReleased()||!GuAd104ColdEntry()){s=EFI_NOT_READY;goto failed;}
  Gu.display_check=GuMsiReleased;Gu.display_ok=TRUE;Gu.armed=TRUE;
  UINT64 mask=0xffffffffffffULL;
  struct pci_dev dev={.vendor=0x10de,.device=0x2704u,.dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffULL},
    .current_state=PCI_D0,.pm_cap=Gu.pm_cap,.refs=1};
  struct gpu_unlock_610_shared shared={0};
  stage="shared-assets";int result=gpu_unlock_610_load_shared(&dev,&shared);
  if(!result) {stage="firmware-transaction";result=gpu_unlock_610_unlock(&dev,&shared);}
  gpu_unlock_610_release_shared(&shared);
  GuLog("Transaction result=%d safe=%u retained=%u started=%u quiesced=%u flrs=%u DMA_live=%u\n",
    result,GuSafeToReturn(),Gu.retained,Gu.started,Gu.quiesced,Gu.flrs,Gu.dma_count);
  if(result||Gu.fault){
    GuLog("transaction incomplete result=%d fault=%u; refusing to continue\n",result,Gu.fault);
    s=EFI_DEVICE_ERROR;goto failed;
  }
  stage="release-exclusive-claim";s=GuMsiFinish();
  if(s!=EFI_SUCCESS||Gu.fault){
    /* GuMsiFinish() closes the exclusive claim first and only then compares the
     * graph, so a non-zero status here usually means "claim released, graph
     * drifted". Treat an unreleased claim or live DMA as fatal; a drifted graph
     * after a clean close is not a reason to strand the machine. */
    GuLog("finish incomplete status=%#llx owned=%u safe=%u\n",
      (unsigned long long)s,(unsigned)Gu.owned,(unsigned)GuSafeToReturn());
    if(Gu.owned||!GuSafeToReturn()){s=EFI_DEVICE_ERROR;goto failed;}
  }
  /* Verify while the log is still open so a refusal is recorded. Previously the
   * close came first, which made the final gate unobservable: a stop here left a
   * log ending in "TRANSACTION OK" and no reason, and an unexplained stop is
   * indistinguishable from a hang for whoever is watching the screen. */
  stage="final-Windows-verification";if(!GuMsiReadyForWindows()){s=EFI_NOT_READY;goto failed;}
  GuLog("TRANSACTION OK: hardware readbacks passed, DMA released; handoff check passed; benchmark still required\n");
  stage="close-USB-log";s=GuBootClose();if(s!=EFI_SUCCESS||Gu.fault)goto failed;
  stage="start-Windows";s=GuBootWindows();
  /* A returned Windows image is not a successful handoff. */
  GuBootHalt();return s==EFI_SUCCESS?EFI_ABORTED:s;
failed:
  if(s==EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",stage,(unsigned long long)s,GuMsiMutated(),Gu.retained,Gu.fault);
  if(Gu.started||Gu.retained||!GuSafeToReturn()) {
    /* Real containment: the GPU is mid-operation or DMA is live. Do not hand
     * this state to Windows. */
    if(Gu.started||Gu.master_attributes_pending)(VOID)GuClearMaster();
    (VOID)GuBootClose();GuBootHalt();return EFI_ABORTED;
  }
  /* Tolerate a refused gate only when a Windows image is actually staged. Before
   * that point GuBootWindows() has nothing to start, so keep the upstream halt. */
  /* Upstream refuses to start Windows from a failed run, and that is correct: a
   * half-released GPU is not a state to hand to a driver. Only the final
   * verification tolerates a negative result, because by then the transaction
   * has completed and the hardware is quiescent.
   * Halt only when something was actually mutated or is still live; otherwise
   * return to the firmware so the boot order can move on. */
  if(GuMsiMutated()||Gu.started||Gu.retained) {
    if(Gu.started||Gu.master_attributes_pending)(VOID)GuClearMaster();
    (VOID)GuBootClose();GuBootHalt();return EFI_ABORTED;
  }
  GuBootSay(L"\r\nRefused before the GPU was touched. Use Windows Boot Manager.\r\n");
  (VOID)GuBootClose();
  return s;
}
