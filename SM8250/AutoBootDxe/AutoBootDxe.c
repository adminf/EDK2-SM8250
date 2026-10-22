/** @file
  AutoBootDxe - diagnostics edition.

  Loads \EFI\BOOT\BOOTAA64.EFI from the first SimpleFileSystem volume that
  has it. On failure: clears the screen, prints a full diagnostic summary
  (volume count, per-volume root dir listing, per-step status) and parks
  forever so the result is easy to photograph.

  Copyright (c) 2026, enuma WoA project. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Guid/EventGroup.h>

STATIC BOOLEAN mAttempted = FALSE;
STATIC BOOLEAN mParked    = FALSE;

#define MAXVOL 8

typedef struct {
  CHAR16  Root[16][32];
  UINTN   RootCount;
  CHAR16  BootFile[64];   // L"OK" or the error
  BOOLEAN OpenVolFail;
  CHAR16  OpenVolErr[24];
} VOL_INFO;

STATIC VOL_INFO  mInfo[MAXVOL];
STATIC UINTN     mVolCount = 0;

STATIC
VOID
ExamineVolume (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs,
  IN UINTN                            Index
  )
{
  EFI_STATUS        Status;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  CHAR16            Buf[64];
  UINTN             Size;

  if (Index >= MAXVOL) {
    return;
  }

  mInfo[Index].BootFile[0] = 0;

  Status = Sfs->OpenVolume (Sfs, &Root);
  if (EFI_ERROR (Status)) {
    mInfo[Index].OpenVolFail = TRUE;
    UnicodeSPrint (mInfo[Index].OpenVolErr, sizeof (mInfo[Index].OpenVolErr), L"%r", Status);
    return;
  }

  mInfo[Index].OpenVolFail = FALSE;

  //
  // root dir listing
  //
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
      Buf[30] = 0;
      UnicodeSPrint (
        mInfo[Index].Root[mInfo[Index].RootCount],
        sizeof (mInfo[Index].Root[0]),
        L"%s",
        Buf
        );
      mInfo[Index].RootCount++;
      if (mInfo[Index].RootCount >= 16) {
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
  }

  Root->Close (Root);
}

STATIC
VOID
ParkWithSummary (
  VOID
  )
{
  UINTN  i, j;

  mParked = TRUE;

  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
  Print (L"==== AutoBoot DIAGNOSTIC SUMMARY ====\n\n");
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
  Print (L"FAT volumes mounted: %d\n\n", mVolCount);

  for (i = 0; i < mVolCount && i < MAXVOL; i++) {
    Print (L"Volume %d:\n", i);
    if (mInfo[i].OpenVolFail) {
      Print (L"  OpenVolume FAILED: %s\n", mInfo[i].OpenVolErr);
      continue;
    }
    Print (L"  root dir (%d entries):\n", mInfo[i].RootCount);
    for (j = 0; j < mInfo[i].RootCount; j++) {
      Print (L"    [%02d] %s\n", j, mInfo[i].Root[j]);
    }
    Print (L"  \\EFI\\BOOT\\BOOTAA64.EFI : %s\n", mInfo[i].BootFile);
    Print (L"\n");
  }

  if (mAttempted) {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGREEN, EFI_BLACK));
    Print (L"A boot attempt was made (see log above the menu).\n");
  } else {
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_RED, EFI_BLACK));
    Print (L"NO volume contained \\EFI\\BOOT\\BOOTAA64.EFI.\n");
  }

  Print (L"\nParked. Power off with 10s power button when photographed.\n");
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

    ExamineVolume (Sfs, Index);
  }

  //
  // second pass: boot from the first volume that has the loader
  //
  for (Index = 0; Index < Count && Index < MAXVOL; Index++) {
    if (mInfo[Index].OpenVolFail || (mInfo[Index].BootFile[0] == 0)) {
      continue;
    }
    if (StrStr (mInfo[Index].BootFile, L"OK") != mInfo[Index].BootFile) {
      continue;
    }

    FileDp = FileDevicePath (Handles[Index], L"\\EFI\\BOOT\\BOOTAA64.EFI");
    Status = gBS->LoadImage (TRUE, gImageHandle, FileDp, NULL, 0, &ImageHandle);
    Print (L"[AutoBoot] LoadImage (volume %d) = %r\n", Index, Status);
    if (EFI_ERROR (Status)) {
      mInfo[Index].BootFile[0] = 0;
      UnicodeSPrint (mInfo[Index].BootFile, sizeof (mInfo[Index].BootFile), L"LOAD FAIL %r", Status);
      continue;
    }

    mAttempted = TRUE;
    Print (L"[AutoBoot] starting Windows Boot Manager...\n");
    Status = gBS->StartImage (ImageHandle, NULL, NULL);
    UnicodeSPrint (mInfo[Index].BootFile, sizeof (mInfo[Index].BootFile), L"STARTED, returned %r", Status);
    break;
  }

  if (Handles != NULL) {
    gBS->FreePool (Handles);
  }

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
