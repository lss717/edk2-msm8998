/** @file
  Publish the Android boot image initrd as a read-only block device.

  The ABL leaves the boot image's ramdisk in memory and describes it in the
  live device tree as /chosen/linux,initrd-start and linux,initrd-end.  The
  edk2-msm8998 platform saves the live DTB pointer at PcdDeviceTreeStore
  (0x83300000 on this tree).  Expose the initrd through EFI_BLOCK_IO so the
  FAT driver can mount the UniXNU payload volume.

  Based on uefi-sheng's ShengBootMemoryDxe, reduced to the initrd handoff.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <libfdt.h>

#define CHEESEBURGER_DTB_STORE  0x83300000ULL
#define RAM_BLOCK_SIZE          512

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return fdt64_to_cpu (ReadUnaligned64 (Data));
  }

  return fdt32_to_cpu (ReadUnaligned32 (Data));
}

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0xA9C6827C, 0x1BD8, 0x410C, { 0x88, 0x5A, 0xC4, 0x65, 0x00, 0xF7, 0xD8, 0x36 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8              *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA  mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS  Status;
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;
  EFI_HANDLE  Handle;

  Chosen = fdt_path_offset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "CHEESEBURGER: no /chosen\n"));
    return;
  }

  StartProp = fdt_getprop (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = fdt_getprop (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "CHEESEBURGER: no initrd in /chosen\n"));
    return;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if ((End <= Start) || (((End - Start) % RAM_BLOCK_SIZE) != 0)) {
    DEBUG ((DEBUG_ERROR, "CHEESEBURGER: initrd 0x%lx-0x%lx is not a whole number of sectors\n", Start, End));
    return;
  }

  mRamBase                = (UINT8 *)(UINTN)Start;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (End - Start) / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, "CHEESEBURGER: initrd block device 0x%lx size 0x%lx %r\n", Start, End - Start, Status));
}

EFI_STATUS
EFIAPI
CheeseburgerBootMemoryDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PHYSICAL_ADDRESS  Dtb;

  Dtb = *(EFI_PHYSICAL_ADDRESS *)(UINTN)CHEESEBURGER_DTB_STORE;
  if ((Dtb == 0) || ((Dtb & 3) != 0) || (fdt_check_header ((VOID *)(UINTN)Dtb) != 0)) {
    DEBUG ((DEBUG_ERROR, "CHEESEBURGER: no device tree at 0x%lx\n", Dtb));
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_ERROR,
    "CHEESEBURGER: device tree at 0x%lx, %u bytes\n",
    Dtb,
    (UINT32)fdt_totalsize ((VOID *)(UINTN)Dtb)
    ));

  PublishInitrd ((VOID *)(UINTN)Dtb);
  return EFI_SUCCESS;
}
