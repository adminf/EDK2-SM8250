/** @file
  AutoBootDxe v5 - deep diagnostic, partition-mapping edition.

  Tries \EFI\BOOT\BOOTAA64.EFI on every SimpleFileSystem volume. On failure:
  clears the screen and prints (1) SFS summary, (2) every BlockIo >= 1GiB with
  its device-path partition number / start LBA / size, BPB fields, OEM+fstype,
  and root-dir sector ASCII when the BPB looks FAT-valid, (3) GPT header and
  entries p33..p39 (name, start, end) read from the largest handle (whole
  disk). Parks forever.

  Copyright (c) 2026, enuma WoA project. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Guid/EventGroup.h>

#define MAXVOL 8

typedef struct {
  BOOLEAN BootOk;
  CHAR16  Root[12][24];
  UINTN   RootCount;
  CHAR16  BootFile[48];
  CHAR16  DpText[64];
  BOOLEAN OpenVolFail;
  CHAR16  OpenVolErr[24];
} VOL_INFO;

STATIC VOL_INFO  mInfo[MAXVOL];
STATIC UINTN     mVolCount = 0;
STATIC BOOLEAN   mAttempted = FALSE;
STATIC BOOLEAN   mParked    = FALSE;

STATIC
VOID
DumpAscii (
  IN UINT8  *P,
  IN UINTN  Len
  )
{
  UINTN   i;
  CHAR16  Ch[2];

  Ch[1] = 0;
  for (i = 0; i < Len; i++) {
    UINT8  C;

    C      = P[i];
    Ch[0]  = ((C >= 0x20) && (C <= 0x7E)) ? (CHAR16)C : L'.';
    Print (L"%s", Ch);
  }
}

//
// Pull the MEDIA_PARTITION node out of the handle's device path.
//
STATIC
BOOLEAN
GetPartitionInfo (
  IN  EFI_HANDLE  Handle,
  OUT UINT32      *PartNum,
  OUT UINT64      *PartStart,
  OUT UINT64      *PartSize
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  BOOLEAN                   Found;
  UINT8                     *P;

  Status = gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&Dp);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Found = FALSE;
  for (Node = Dp; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((Node->Type == MEDIA_DEVICE_PATH) && (Node->SubType == MEDIA_HARDDRIVE_DP) && (DevicePathNodeLength (Node) >= 42)) {
      P         = (UINT8 *)Node;
      *PartNum  = *(UINT32 *)(P + 4);
      *PartStart = *(UINT64 *)(P + 8);
      *PartSize = *(UINT64 *)(P + 16);
      Found     = TRUE;
    }
  }

  return Found;
}

//
// Dump BPB fields of a partition; root-dir ASCII when the BPB is FAT-like.
//
STATIC
VOID
DumpBpb (
  IN EFI_BLOCK_IO_PROTOCOL  *Bio
  )
{
  EFI_STATUS  Status;
  UINT32      Bs;
  UINT8       *P;
  UINT16      Bps;
  UINT16      Resv;
  UINT8       Spc;
  UINT8       NumFats;
  UINT32      FatSz;
  UINT32      TotSec;
  UINT32      RootClus;
  UINT64      RootLba;
  CHAR8       Asc[9];

  Bs = Bio->Media->BlockSize;
  P  = AllocatePool (Bs * 2);
  if (P == NULL) {
    return;
  }

  Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, Bs, P);
  if (EFI_ERROR (Status)) {
    Print (L"    LBA0 read FAIL %r\n", Status);
    FreePool (P);
    return;
  }

  CopyMem (Asc, P + 3, 8);
  Asc[8] = 0;
  Bps      = *(UINT16 *)(P + 0x0B);
  Spc      = P[0x0D];
  Resv     = *(UINT16 *)(P + 0x0E);
  NumFats  = P[0x10];
  TotSec   = *(UINT32 *)(P + 0x20);
  FatSz    = *(UINT32 *)(P + 0x24);
  RootClus = *(UINT32 *)(P + 0x2C);

  Print (L"    OEM:%a bps=%d spc=%d resv=%d nfat=%d fatsz=%d totsec=%d root=%d\n",
         Asc, Bps, Spc, Resv, NumFats, FatSz, TotSec, RootClus);
  CopyMem (Asc, P + 0x52, 8);
  Asc[8] = 0;
  Print (L"    fstype@52:%a\n", Asc);

  if ((Bps == 512) || (Bps == Bs)) {
    if ((FatSz > 0) && (NumFats >= 1) && (NumFats <= 8)) {
      RootLba = ((UINT64)Resv + (UINT64)NumFats * FatSz) * Bps / Bs;
      Status  = Bio->ReadBlocks (Bio, Bio->Media->MediaId, RootLba, Bs, P);
      if (!EFI_ERROR (Status)) {
        Print (L"    root@%lu: ", RootLba);
        DumpAscii (P, 64);
        Print (L"\n");
      }
    }
  }

  FreePool (P);
}

//
// Scan the first 16MB of the ESP partition for FAT boot-sector signatures.
//
STATIC
VOID
ScanEsp (
  IN EFI_BLOCK_IO_PROTOCOL  *Bio
  )
{
  EFI_STATUS  Status;
  UINT32      Bs;
  UINT8       *Buf;
  UINT32      i;
  UINTN       Hits;
  UINT16      Bps;
  CHAR8       Asc[9];

  Bs = Bio->Media->BlockSize;
  if (Bs == 0) {
    return;
  }

  Buf = AllocatePool (Bs * 4096);
  if (Buf == NULL) {
    Print (L"    scan alloc FAIL\n");
    return;
  }

  Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, Bs * 4096, Buf);
  if (EFI_ERROR (Status)) {
    Print (L"    scan read FAIL %r\n", Status);
    FreePool (Buf);
    return;
  }

  Print (L"    LBA0[0..15]: ");
  DumpAscii (Buf, 16);
  Print (L"\n");

  Hits = 0;
  for (i = 0; i < 4096; i++) {
    UINT8  *P;

    P   = Buf + (UINTN)i * Bs;
    Bps = *(UINT16 *)(P + 0x0B);
    if (((P[510] == 0x55) && (P[511] == 0xAA)) || (Bps == 512)) {
      CopyMem (Asc, P + 3, 8);
      Asc[8] = 0;
      Print (
        L"    FAT sig@LBA%d OEM:%a bps=%d fatsz=%d totsec=%d\n",
        i, Asc, Bps, *(UINT32 *)(P + 0x24), *(UINT32 *)(P + 0x20)
        );
      if (++Hits >= 5) {
        break;
      }
    }
  }

  if (Hits == 0) {
    Print (L"    no FAT sig in first %d LBAs\n", 4096);
  }

  FreePool (Buf);
}

//
// GPT header + entries p33..p39 from the whole-disk handle.
//
STATIC
VOID
DumpGpt (
  IN EFI_BLOCK_IO_PROTOCOL  *Disk
  )
{
  EFI_STATUS  Status;
  UINT32      Bs;
  UINT8       *Hdr;
  UINT8       *Arr;
  UINT64      EntryLba;
  UINT32      Num;
  UINT32      Esz;
  UINT32      i;
  UINT64      Start;
  UINT64      End;
  CHAR16      Name[9];
  UINT8       *E;

  Bs  = Disk->Media->BlockSize;
  Hdr = AllocatePool (Bs);
  if (Hdr == NULL) {
    return;
  }

  Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, 1, Bs, Hdr);
  if (EFI_ERROR (Status)) {
    Print (L"  GPT hdr read FAIL %r\n", Status);
    FreePool (Hdr);
    return;
  }

  if (CompareMem (Hdr, "EFI PART", 8) != 0) {
    Print (L"  GPT hdr BAD sig: ");
    DumpAscii (Hdr, 8);
    Print (L"\n");
    FreePool (Hdr);
    return;
  }

  EntryLba = *(UINT64 *)(Hdr + 0x48);
  Num      = *(UINT32 *)(Hdr + 0x50);
  Esz      = *(UINT32 *)(Hdr + 0x54);
  Print (L"  GPT OK: %d x %d B @LBA%lu\n", Num, Esz, EntryLba);

  if ((Esz == 0) || (Esz > 1024) || (Num > 128)) {
    FreePool (Hdr);
    return;
  }

  Arr = AllocatePool (Bs * 4);
  if (Arr == NULL) {
    FreePool (Hdr);
    return;
  }

  Status = Disk->ReadBlocks (Disk, Disk->Media->MediaId, EntryLba, Bs * 4, Arr);
  if (EFI_ERROR (Status)) {
    Print (L"  GPT arr read FAIL %r\n", Status);
  } else {
    for (i = 32; (i < Num) && (i < 39); i++) {
      E = Arr + (UINTN)i * Esz;
      if (*(UINT64 *)E == 0) {
        continue;
      }

      Start = *(UINT64 *)(E + 0x10);
      End   = *(UINT64 *)(E + 0x18);
      CopyMem (Name, E + 0x38, 16);
      Name[8] = 0;
      Print (L"  p%d: [%lu..%lu] %s\n", i + 1, Start, End, Name);
    }
  }

  FreePool (Arr);
  FreePool (Hdr);
}

//
// Raw block view of everything big.
//
STATIC
VOID
DeepDiag (
  VOID
  )
{
  EFI_STATUS             Status;
  UINTN                  Count;
  UINTN                  i;
  EFI_HANDLE             *Handles;
  EFI_BLOCK_IO_PROTOCOL  *Bio;
  UINT64                 Cap;
  UINT64                 MaxCap;
  EFI_BLOCK_IO_PROTOCOL  *Disk;
  UINT32                 PartNum;
  UINT64                 PartStart;
  UINT64                 PartSize;
  BOOLEAN                GotPart;

  Count   = 0;
  Handles = NULL;
  Disk    = NULL;
  MaxCap  = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Print (L"BlockIo locate FAIL %r\n", Status);
    return;
  }

  Print (L"BlockIo handles: %d (>=1GiB shown)\n", Count);
  for (i = 0; i < Count; i++) {
    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&Bio);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Cap = (Bio->Media->LastBlock + 1) * (UINT64)Bio->Media->BlockSize;
    if (Cap < (1ULL << 30)) {
      continue;
    }

    GotPart = GetPartitionInfo (Handles[i], &PartNum, &PartStart, &PartSize);
    if (GotPart) {
      Print (
        L"  [%d] part#%d start=%lu cap=%dMB\n",
        i, PartNum, PartStart, (UINT32)(Cap >> 20)
        );
    } else {
      Print (L"  [%d] DISK cap=%dMB bs=%d\n", i, (UINT32)(Cap >> 20), Bio->Media->BlockSize);
    }

    DumpBpb (Bio);

    if (GotPart && (PartStart == 31522816)) {
      ScanEsp (Bio);
    }

    if (Cap > MaxCap) {
      MaxCap = Cap;
      Disk   = Bio;
    }
  }

  if (Disk != NULL) {
    Print (L"Whole-disk GPT (cap=%dMB):\n", (UINT32)(MaxCap >> 20));
    DumpGpt (Disk);
  }

  FreePool (Handles);
}

STATIC
VOID
ExamineVolume (
  IN EFI_HANDLE                      Handle,
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs,
  IN UINTN                           Index
  )
{
  EFI_STATUS                Status;
  EFI_FILE_PROTOCOL         *Root;
  EFI_FILE_PROTOCOL         *File;
  CHAR16                    Buf[64];
  UINTN                     Size;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  CHAR16                    *Text;
  UINTN                     k;

  if (Index >= MAXVOL) {
    return;
  }

  mInfo[Index].BootFile[0] = 0;
  mInfo[Index].BootOk      = FALSE;
  mInfo[Index].DpText[0]   = 0;

  Status = gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&Dp);
  if (!EFI_ERROR (Status)) {
    Text = ConvertDevicePathToText (Dp, FALSE, FALSE);
    if (Text != NULL) {
      for (k = 0; (k < 63) && (Text[k] != 0); k++) {
        mInfo[Index].DpText[k] = Text[k];
      }

      mInfo[Index].DpText[k] = 0;
      FreePool (Text);
    }
  }

  Status = Sfs->OpenVolume (Sfs, &Root);
  if (EFI_ERROR (Status)) {
    mInfo[Index].OpenVolFail = TRUE;
    UnicodeSPrint (mInfo[Index].OpenVolErr, sizeof (mInfo[Index].OpenVolErr), L"%r", Status);
    return;
  }

  mInfo[Index].OpenVolFail = FALSE;

  Status = Root->Open (Root, &File, L".", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    mInfo[Index].RootCount = 0;
    for ( ; ; ) {
      Size = sizeof (Buf);
      Status = File->Read (File, &Size, Buf);
      if (EFI_ERROR (Status) || (Size == 0)) {
        break;
      }

      if (Buf[0] == L'.') {
        continue;
      }

      Buf[22] = 0;
      UnicodeSPrint (
        mInfo[Index].Root[mInfo[Index].RootCount],
        sizeof (mInfo[Index].Root[0]),
        L"%s",
        Buf
        );
      mInfo[Index].RootCount++;
      if (mInfo[Index].RootCount >= 12) {
        break;
      }
    }

    File->Close (File);
  }

  Status = Root->Open (
                   Root,
                   &File,
                   L"\\EFI\\BOOT\\BOOTAA64.EFI",
                   EFI_FILE_MODE_READ,
                   0
                   );
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (mInfo[Index].BootFile, sizeof (mInfo[Index].BootFile), L"NOT FOUND (%r)", Status);
  } else {
    File->Close (File);
    UnicodeSPrint (mInfo[Index].BootFile, sizeof (mInfo[Index].BootFile), L"OK");
    mInfo[Index].BootOk = TRUE;
  }

  Root->Close (Root);
}

STATIC
VOID
ParkWithSummary (
  VOID
  )
{
  UINTN  i;
  UINTN  j;

  mParked = TRUE;

  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
  Print (L"==== DIAG v5: PART MAP ====\n\n");

  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
  Print (L"SFS volumes: %d\n", mVolCount);
  for (i = 0; i < mVolCount && i < MAXVOL; i++) {
    Print (L"Volume %d:\n", i);
    if (mInfo[i].OpenVolFail) {
      Print (L"  OpenVolume FAILED: %s\n", mInfo[i].OpenVolErr);
      continue;
    }

    Print (L"  root (%d):", mInfo[i].RootCount);
    for (j = 0; j < mInfo[i].RootCount; j++) {
      Print (L" %s |", mInfo[i].Root[j]);
    }

    Print (L"\n  BOOTAA64: %s\n  DP: %s\n", mInfo[i].BootFile, mInfo[i].DpText);
  }

  Print (L"\n-- block view --\n");
  DeepDiag ();

  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_RED, EFI_BLACK));
  Print (L"\nNO BOOTAA64. Parked.\n");

  for ( ; ; ) {
    gBS->Stall (1000000);
  }
}

STATIC
VOID
EFIAPI
TryBootAllVolumes (
  VOID
  )
{
  EFI_STATUS                       Status;
  UINTN                            Index;
  UINTN                            Count;
  EFI_HANDLE                       *Handles;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_DEVICE_PATH_PROTOCOL         *FileDp;
  EFI_HANDLE                       ImageHandle;

  if (mAttempted || mParked) {
    return;
  }

  Count   = 0;
  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  if (EFI_ERROR (Status) || (Count == 0)) {
    return;
  }

  mVolCount = Count;
  if (mVolCount > MAXVOL) {
    mVolCount = MAXVOL;
  }

  Print (L"[AutoBoot] found %d FAT volume(s)\n", Count);

  for (Index = 0; Index < Count && Index < MAXVOL; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    ExamineVolume (Handles[Index], Sfs, Index);
  }

  for (Index = 0; Index < Count && Index < MAXVOL; Index++) {
    if (!mInfo[Index].BootOk) {
      continue;
    }

    FileDp = FileDevicePath (Handles[Index], L"\\EFI\\BOOT\\BOOTAA64.EFI");
    Status = gBS->LoadImage (TRUE, gImageHandle, FileDp, NULL, 0, &ImageHandle);
    Print (L"[AutoBoot] LoadImage (volume %d) = %r\n", Index, Status);
    if (EFI_ERROR (Status)) {
      mInfo[Index].BootOk   = FALSE;
      UnicodeSPrint (mInfo[Index].BootFile, sizeof (mInfo[Index].BootFile), L"LOAD FAIL %r", Status);
      continue;
    }

    mAttempted = TRUE;
    Print (L"[AutoBoot] starting Windows Boot Manager...\n");
    Status = gBS->StartImage (ImageHandle, NULL, NULL);
    UnicodeSPrint (mInfo[Index].BootFile, sizeof (mInfo[Index].BootFile), L"STARTED, returned %r", Status);
    break;
  }

  FreePool (Handles);

  if (!mAttempted) {
    ParkWithSummary ();
  }
}

STATIC
VOID
EFIAPI
OnSfsNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TryBootAllVolumes ();
}

EFI_STATUS
EFIAPI
AutoBootDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;
  VOID        *Reg;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnSfsNotify,
                  NULL,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiSimpleFileSystemProtocolGuid,
                  Event,
                  &Reg
                  );
  if (!EFI_ERROR (Status)) {
    TryBootAllVolumes ();
  }

  return Status;
}
