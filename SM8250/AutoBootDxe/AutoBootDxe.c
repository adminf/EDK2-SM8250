/** @file
  AutoBootDxe - Automatically load \EFI\BOOT\BOOTAA64.EFI from the first
  SimpleFileSystem volume as soon as it becomes available, with on-screen
  status output. Works around BdsDxe falling back to the FrontPage menu
  without attempting boot.

  Copyright (c) 2026, enuma WoA project. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Guid/EventGroup.h>

STATIC BOOLEAN mAttempted = FALSE;

STATIC
VOID
ListRootDir (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs,
  IN UINTN                            Index
  )
{
  EFI_STATUS        Status;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  CHAR16            Buf[64];
  UINTN             Size;
  UINTN             Shown;

  Status = Sfs->OpenVolume (Sfs, &Root);
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = Root->Open (Root, &File, L".", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Print (L"[AutoBoot] vol %d: cannot open root dir = %r\n", Index, Status);
    Root->Close (Root);
    return;
  }

  Print (L"[AutoBoot] vol %d root dir:\n", Index);
  Shown = 0;
  for ( ; ; ) {
    Size = sizeof (Buf);
    Status = File->Read (File, &Size, Buf);
    if (EFI_ERROR (Status) || (Size == 0)) {
      break;
    }
    if (Buf[0] == L'.') {
      continue;
    }
    Buf[31] = 0;
    Print (L"    %s\n", Buf);
    if (++Shown >= 12) {
      Print (L"    ...\n");
      break;
    }
  }

  File->Close (File);
  Root->Close (Root);
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
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_DEVICE_PATH_PROTOCOL         *FileDp;
  EFI_HANDLE                       ImageHandle;

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

  Print (L"[AutoBoot] found %d FAT volume(s)\n", Count);
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    ListRootDir (Sfs, Index);

    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status)) {
      Print (L"[AutoBoot] volume %d: OpenVolume = %r\n", Index, Status);
      continue;
    }

    Status = Root->Open (
                     Root,
                     &File,
                     L"\\EFI\\BOOT\\BOOTAA64.EFI",
                     EFI_FILE_MODE_READ,
                     0
                     );
    if (EFI_ERROR (Status)) {
      Print (L"[AutoBoot] volume %d: no \\EFI\\BOOT\\BOOTAA64.EFI (%r)\n", Index, Status);
      Root->Close (Root);
      continue;
    }

    File->Close (File);
    Root->Close (Root);

    FileDp = FileDevicePath (Handles[Index], L"\\EFI\\BOOT\\BOOTAA64.EFI");
    Status = gBS->LoadImage (TRUE, gImageHandle, FileDp, NULL, 0, &ImageHandle);
    Print (L"[AutoBoot] LoadImage BOOTAA64.EFI (volume %d) = %r\n", Index, Status);
    if (EFI_ERROR (Status)) {
      continue;
    }

    mAttempted = TRUE;
    Print (L"[AutoBoot] starting Windows Boot Manager...\n");
    Status = gBS->StartImage (ImageHandle, NULL, NULL);
    Print (L"[AutoBoot] StartImage returned = %r\n", Status);
    if (!EFI_ERROR (Status)) {
      return;
    }
  }

  if (Handles != NULL) {
    gBS->FreePool (Handles);
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

/**
  Entry point: register a protocol notify on SimpleFileSystem.
**/
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
  //
  // There may already be mounted volumes at our dispatch time.
  //
  if (!EFI_ERROR (Status)) {
    TryBootAllVolumes ();
  }

  return Status;
}
