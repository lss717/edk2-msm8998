/** @file
  Print the board facts that the B1 device test has to confirm.

  Left out of the firmware volume on purpose: UniXNU launches it from the
  payload ESP, so the firmware under test stays exactly as built.  It prints
  the memory map total, the graphics output mode and every whole-disk block
  device with its logical block size and the GPT userdata bounds.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>

#define GPT_HEADER_LBA  1
#define GPT_ENTRY_MAX   (4 * 1024 * 1024)

STATIC UINT64   mRamMiB;
STATIC UINT32   mGopWidth;
STATIC UINT32   mGopHeight;
STATIC UINT32   mGopStride;
STATIC UINT64   mGopBase;
STATIC BOOLEAN  mUserdataFound;
STATIC UINT64   mUserdataFirst;
STATIC UINT64   mUserdataLast;
STATIC UINT32   mUserdataBlockSize;

STATIC
VOID
ClearScreen (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = { 0, 0, 0, 0 };

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return;
  }

  Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0,
            Gop->Mode->Info->HorizontalResolution,
            Gop->Mode->Info->VerticalResolution, 0);
}

STATIC
VOID
PrintMemory (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN                 Size;
  UINTN                 MapKey;
  UINTN                 DescriptorSize;
  UINT32                DescriptorVersion;
  UINT64                Total;
  UINTN                 Count;
  UINTN                 Index;

  Size = 0;
  Status = gBS->GetMemoryMap (&Size, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Print (L"MEM: GetMemoryMap size %r\r\n", Status);
    return;
  }

  Size += 4 * DescriptorSize;
  Map   = AllocatePool (Size);
  if (Map == NULL) {
    Print (L"MEM: out of memory\r\n");
    return;
  }

  Status = gBS->GetMemoryMap (&Size, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (EFI_ERROR (Status)) {
    Print (L"MEM: GetMemoryMap %r\r\n", Status);
    FreePool (Map);
    return;
  }

  Total = 0;
  Count = Size / DescriptorSize;
  for (Index = 0; Index < Count; Index++) {
    EFI_MEMORY_DESCRIPTOR *Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescriptorSize);
    if (Entry->Type != EfiReservedMemoryType) {
      Total += Entry->NumberOfPages;
    }
  }

  mRamMiB = Total * EFI_PAGE_SIZE / (1024 * 1024);
  Print (L"MEM: %lu MiB in %lu descriptors (map key 0x%lx)\r\n",
         mRamMiB, (UINT64)Count, (UINT64)MapKey);
  FreePool (Map);
}

STATIC
VOID
PrintGop (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    Print (L"GOP: not available (%r)\r\n", Status);
    return;
  }

  mGopWidth  = Gop->Mode->Info->HorizontalResolution;
  mGopHeight = Gop->Mode->Info->VerticalResolution;
  mGopStride = Gop->Mode->Info->PixelsPerScanLine;
  mGopBase   = Gop->Mode->FrameBufferBase;
  Print (L"GOP: %ux%u stride %u fmt %u base 0x%lx size 0x%lx\r\n",
         mGopWidth, mGopHeight, mGopStride,
         (UINT32)Gop->Mode->Info->PixelFormat,
         mGopBase, (UINT64)Gop->Mode->FrameBufferSize);
}

STATIC
VOID
PrintGpt (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  EFI_STATUS  Status;
  UINT8       *Sector;
  UINT8       *Entries;
  UINT64      EntriesLba;
  UINT32      EntryCount;
  UINT32      EntrySize;
  UINT64      EntryBytes;
  UINTN       ReadSize;
  UINT32      Index;

  Sector = AllocatePages (EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize));
  if (Sector == NULL) {
    return;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, GPT_HEADER_LBA,
                                BlockIo->Media->BlockSize, Sector);
  if (EFI_ERROR (Status) || (CompareMem (Sector, "EFI PART", 8) != 0)) {
    FreePages (Sector, EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize));
    return;
  }

  EntriesLba = *(UINT64 *)(Sector + 72);
  EntryCount = *(UINT32 *)(Sector + 80);
  EntrySize  = *(UINT32 *)(Sector + 84);
  if ((EntrySize < 128) || (EntryCount == 0) || (EntryCount > 1024)) {
    Print (L"  GPT: entry count %u size %u, not used\r\n", EntryCount, EntrySize);
    FreePages (Sector, EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize));
    return;
  }

  EntryBytes = (UINT64)EntryCount * EntrySize;
  if (EntryBytes > GPT_ENTRY_MAX) {
    Print (L"  GPT: entries too large (%lu bytes)\r\n", EntryBytes);
    FreePages (Sector, EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize));
    return;
  }

  ReadSize = (UINTN)EntryBytes;
  ReadSize = (ReadSize + BlockIo->Media->BlockSize - 1) / BlockIo->Media->BlockSize * BlockIo->Media->BlockSize;
  Entries  = AllocatePages (EFI_SIZE_TO_PAGES (ReadSize));
  if (Entries == NULL) {
    FreePages (Sector, EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize));
    return;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, EntriesLba, ReadSize, Entries);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < EntryCount; Index++) {
      UINT8     *Entry = Entries + (UINTN)Index * EntrySize;
      UINT64    First;
      UINT64    Last;
      BOOLEAN   Empty;
      CHAR16    Name[37];
      UINTN     Char;

      Empty = TRUE;
      for (Char = 0; Char < 16; Char++) {
        if (Entry[Char] != 0) {
          Empty = FALSE;
        }
      }
      if (Empty) {
        continue;
      }

      for (Char = 0; Char < 36; Char++) {
        Name[Char] = (CHAR16)(Entry[56 + Char * 2] | (Entry[57 + Char * 2] << 8));
        if (Name[Char] == 0) {
          break;
        }
      }
      Name[Char] = 0;
      if ((StrCmp (Name, L"userdata") != 0) && (StrCmp (Name, L"USERDATA") != 0)) {
        continue;
      }

      First = *(UINT64 *)(Entry + 32);
      Last  = *(UINT64 *)(Entry + 40);
      mUserdataFound     = TRUE;
      mUserdataFirst     = First;
      mUserdataLast      = Last;
      mUserdataBlockSize = BlockIo->Media->BlockSize;
      Print (L"  userdata: first 0x%lx last 0x%lx (%lu blocks, %lu MiB)\r\n",
             First, Last, Last - First + 1,
             (Last - First + 1) * BlockIo->Media->BlockSize / (1024 * 1024));
    }
  }

  FreePages (Entries, EFI_SIZE_TO_PAGES (ReadSize));
  FreePages (Sector, EFI_SIZE_TO_PAGES (BlockIo->Media->BlockSize));
}

STATIC
VOID
PrintDisks (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             *Handles;
  UINTN                  Count;
  UINTN                  Index;
  UINTN                  Disk;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"BLK: no block devices (%r)\r\n", Status);
    return;
  }

  Disk = 0;
  for (Index = 0; Index < Count; Index++) {
    EFI_BLOCK_IO_PROTOCOL  *BlockIo;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo->Media == NULL) || BlockIo->Media->LogicalPartition) {
      continue;
    }

    Print (L"BLK%u: block %u bytes, %lu blocks, %lu MiB%s%s\r\n",
           (UINT64)Disk,
           BlockIo->Media->BlockSize,
           BlockIo->Media->LastBlock + 1,
           (BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize / (1024 * 1024),
           BlockIo->Media->ReadOnly ? L", read-only" : L"",
           BlockIo->Media->RemovableMedia ? L", removable" : L"");
    PrintGpt (BlockIo);
    Disk++;
  }

  FreePool (Handles);
  if (Disk == 0) {
    Print (L"BLK: no whole-disk block devices\r\n");
  }
}

EFI_STATUS
EFIAPI
CheeseburgerInfoEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN  Index;

  ClearScreen ();
  Print (L"\r\n");
  Print (L"CHEESEBURGER INFO\r\n");
  Print (L"-----------------\r\n");
  PrintMemory ();
  PrintGop ();
  PrintDisks ();
  Print (L"-----------------\r\n");
  Print (L"SUMMARY\r\n");
  Print (L"  RAM %lu MiB, GOP %ux%u stride %u base 0x%lx\r\n",
         mRamMiB, mGopWidth, mGopHeight, mGopStride, mGopBase);
  if (mUserdataFound) {
    Print (L"  userdata first 0x%lx last 0x%lx, block %u bytes\r\n",
           mUserdataFirst, mUserdataLast, mUserdataBlockSize);
  } else {
    Print (L"  userdata not found in any GPT\r\n");
  }

  Print (L"-----------------\r\n");
  Print (L"Rebooting in 180 s; power off to stop now.\r\n");

  //
  // Stay on the screen long enough to be photographed.  There is no
  // guaranteed keyboard, so do not wait for input.
  //
  for (Index = 0; Index < 180; Index++) {
    gBS->Stall (1000000);
  }

  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  return EFI_SUCCESS;
}
