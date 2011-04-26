/*++ @file  NorFlashFvbDxe.c

 Copyright (c) 2011, ARM Ltd. All rights reserved.<BR>
 This program and the accompanying materials
 are licensed and made available under the terms and conditions of the BSD License
 which accompanies this distribution.  The full text of the license may be found at
 http://opensource.org/licenses/bsd-license.php

 THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

 --*/

#include <PiDxe.h>

#include <Library/PcdLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Guid/VariableFormat.h>
#include <Guid/SystemNvDataGuid.h>

#include "NorFlashDxe.h"


///
/// The Firmware Volume Block Protocol is the low-level interface
/// to a firmware volume. File-level access to a firmware volume
/// should not be done using the Firmware Volume Block Protocol.
/// Normal access to a firmware volume must use the Firmware
/// Volume Protocol. Typically, only the file system driver that
/// produces the Firmware Volume Protocol will bind to the
/// Firmware Volume Block Protocol.
///

/**
  Initialises the FV Header and Variable Store Header
  to support variable operations.

  @param[in]  Ptr - Location to initialise the headers

**/
EFI_STATUS
InitializeFvAndVariableStoreHeaders (
  IN NOR_FLASH_INSTANCE *Instance
  )
{
  EFI_STATUS                          Status;
  VOID*                               Headers;
  UINTN                               HeadersLength;
  EFI_FIRMWARE_VOLUME_HEADER          *FirmwareVolumeHeader;
  VARIABLE_STORE_HEADER               *VariableStoreHeader;

  if (!Instance->Initialized) {
    Instance->Initialize(Instance);
  }

  HeadersLength = sizeof(EFI_FIRMWARE_VOLUME_HEADER) + sizeof(EFI_FV_BLOCK_MAP_ENTRY) + sizeof(VARIABLE_STORE_HEADER);
  Headers = AllocateZeroPool(HeadersLength);

  //
  // EFI_FIRMWARE_VOLUME_HEADER
  //
  FirmwareVolumeHeader = (EFI_FIRMWARE_VOLUME_HEADER*)Headers;
  CopyGuid (&FirmwareVolumeHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid);
  FirmwareVolumeHeader->FvLength = Instance->Media.BlockSize * (Instance->Media.LastBlock + 1);
  FirmwareVolumeHeader->Signature = EFI_FVH_SIGNATURE;
  FirmwareVolumeHeader->Attributes = (EFI_FVB_ATTRIBUTES_2) (
                                          EFI_FVB2_READ_ENABLED_CAP   | // Reads may be enabled
                                          EFI_FVB2_READ_STATUS        | // Reads are currently enabled
                                          EFI_FVB2_STICKY_WRITE       | // A block erase is required to flip bits into EFI_FVB2_ERASE_POLARITY
                                          EFI_FVB2_MEMORY_MAPPED      | // It is memory mapped
                                          EFI_FVB2_ERASE_POLARITY     | // After erasure all bits take this value (i.e. '1')
                                          EFI_FVB2_WRITE_STATUS       | // Writes are currently enabled
                                          EFI_FVB2_WRITE_ENABLED_CAP    // Writes may be enabled
                                      );
  FirmwareVolumeHeader->HeaderLength = sizeof(EFI_FIRMWARE_VOLUME_HEADER) + sizeof(EFI_FV_BLOCK_MAP_ENTRY);
  FirmwareVolumeHeader->Revision = EFI_FVH_REVISION;
  FirmwareVolumeHeader->BlockMap[0].NumBlocks = Instance->Media.LastBlock + 1;
  FirmwareVolumeHeader->BlockMap[0].Length      = Instance->Media.BlockSize;
  FirmwareVolumeHeader->BlockMap[1].NumBlocks = 0;
  FirmwareVolumeHeader->BlockMap[1].Length      = 0;
  FirmwareVolumeHeader->Checksum = CalculateCheckSum16 ((UINT16*)FirmwareVolumeHeader,FirmwareVolumeHeader->HeaderLength);

  //
  // VARIABLE_STORE_HEADER
  //
  VariableStoreHeader = (VARIABLE_STORE_HEADER*)((UINT32)Headers + FirmwareVolumeHeader->HeaderLength);
  CopyGuid (&VariableStoreHeader->Signature, &gEfiVariableGuid);
  VariableStoreHeader->Size = PcdGet32(PcdFlashNvStorageVariableSize) - FirmwareVolumeHeader->HeaderLength;
  VariableStoreHeader->Format            = VARIABLE_STORE_FORMATTED;
  VariableStoreHeader->State             = VARIABLE_STORE_HEALTHY;

  // Install the combined super-header in the NorFlash
  Status = FvbWrite(&Instance->FvbProtocol, 0, 0, &HeadersLength, Headers );

  FreePool(Headers);
  return Status;
}

/**
  Check the integrity of firmware volume header.

  @param[in] FwVolHeader - A pointer to a firmware volume header

  @retval  EFI_SUCCESS   - The firmware volume is consistent
  @retval  EFI_NOT_FOUND - The firmware volume has been corrupted.

**/
EFI_STATUS
ValidateFvHeader (
  IN  NOR_FLASH_INSTANCE *Instance
  )
{
  UINT16                      Checksum;
  EFI_FIRMWARE_VOLUME_HEADER  *FwVolHeader;
  VARIABLE_STORE_HEADER       *VariableStoreHeader;
  UINTN                       VariableStoreLength;

  FwVolHeader = (EFI_FIRMWARE_VOLUME_HEADER*)Instance->BaseAddress;

  //
  // Verify the header revision, header signature, length
  // Length of FvBlock cannot be 2**64-1
  // HeaderLength cannot be an odd number
  //
  if (     ( FwVolHeader->Revision      != EFI_FVH_REVISION     )
        || ( FwVolHeader->Signature     != EFI_FVH_SIGNATURE    )
        || ( FwVolHeader->FvLength      != Instance->Media.BlockSize * (Instance->Media.LastBlock + 1) )
      ) {
    DEBUG ((EFI_D_ERROR, "ValidateFvHeader: No Firmware Volume header present\n"));
    return EFI_NOT_FOUND;
  }

  // Check the Firmware Volume Guid
  if( CompareGuid (&FwVolHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid) == FALSE ) {
    DEBUG ((EFI_D_ERROR, "ValidateFvHeader: Firmware Volume Guid non-compatible\n"));
    return EFI_NOT_FOUND;
  }

  // Verify the header checksum
  Checksum = CalculateSum16((UINT16*)FwVolHeader, FwVolHeader->HeaderLength);
  if (Checksum != 0) {
    DEBUG ((EFI_D_ERROR, "ValidateFvHeader: FV checksum is invalid (Checksum:0x%X)\n",Checksum));
    return EFI_NOT_FOUND;
  }

  VariableStoreHeader = (VARIABLE_STORE_HEADER*)((UINT32)FwVolHeader + FwVolHeader->HeaderLength);

  // Check the Variable Store Guid
  if( CompareGuid (&VariableStoreHeader->Signature, &gEfiVariableGuid) == FALSE ) {
    DEBUG ((EFI_D_ERROR, "ValidateFvHeader: Variable Store Guid non-compatible\n"));
    return EFI_NOT_FOUND;
  }

  VariableStoreLength = PcdGet32 (PcdFlashNvStorageVariableSize) - FwVolHeader->HeaderLength;
  if (VariableStoreHeader->Size != VariableStoreLength) {
    DEBUG ((EFI_D_ERROR, "ValidateFvHeader: Variable Store Length does not match\n"));
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/**
 The GetAttributes() function retrieves the attributes and
 current settings of the block.

 @param This         Indicates the EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL instance.

 @param Attributes   Pointer to EFI_FVB_ATTRIBUTES_2 in which the attributes and
                     current settings are returned.
                     Type EFI_FVB_ATTRIBUTES_2 is defined in EFI_FIRMWARE_VOLUME_HEADER.

 @retval EFI_SUCCESS The firmware volume attributes were returned.

 **/
EFI_STATUS
EFIAPI
FvbGetAttributes(
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL    *This,
  OUT       EFI_FVB_ATTRIBUTES_2                   *Attributes
  )
{
  EFI_FVB_ATTRIBUTES_2  FlashFvbAttributes;
  NOR_FLASH_INSTANCE *Instance;

  Instance = INSTANCE_FROM_FVB_THIS(This);

  FlashFvbAttributes = (EFI_FVB_ATTRIBUTES_2) (

      EFI_FVB2_READ_ENABLED_CAP | // Reads may be enabled
      EFI_FVB2_READ_STATUS      | // Reads are currently enabled
      EFI_FVB2_STICKY_WRITE     | // A block erase is required to flip bits into EFI_FVB2_ERASE_POLARITY
      EFI_FVB2_MEMORY_MAPPED    | // It is memory mapped
      EFI_FVB2_ERASE_POLARITY     // After erasure all bits take this value (i.e. '1')

      );

  // Check if it is write protected
  if (Instance->Media.ReadOnly != TRUE) {

    FlashFvbAttributes = FlashFvbAttributes         |
                         EFI_FVB2_WRITE_STATUS      | // Writes are currently enabled
                         EFI_FVB2_WRITE_ENABLED_CAP;  // Writes may be enabled
  }

  *Attributes = FlashFvbAttributes;

  DEBUG ((DEBUG_BLKIO, "FvbGetAttributes(0x%X)\n", *Attributes));

  return EFI_SUCCESS;
}

/**
 The SetAttributes() function sets configurable firmware volume attributes
 and returns the new settings of the firmware volume.


 @param This                     Indicates the EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL instance.

 @param Attributes               On input, Attributes is a pointer to EFI_FVB_ATTRIBUTES_2
                                 that contains the desired firmware volume settings.
                                 On successful return, it contains the new settings of
                                 the firmware volume.
                                 Type EFI_FVB_ATTRIBUTES_2 is defined in EFI_FIRMWARE_VOLUME_HEADER.

 @retval EFI_SUCCESS             The firmware volume attributes were returned.

 @retval EFI_INVALID_PARAMETER   The attributes requested are in conflict with the capabilities
                                 as declared in the firmware volume header.

 **/
EFI_STATUS
EFIAPI
FvbSetAttributes(
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN OUT    EFI_FVB_ATTRIBUTES_2                 *Attributes
  )
{
  DEBUG ((DEBUG_BLKIO, "FvbSetAttributes(0x%X) is not supported\n",*Attributes));
  return EFI_UNSUPPORTED;
}

/**
 The GetPhysicalAddress() function retrieves the base address of
 a memory-mapped firmware volume. This function should be called
 only for memory-mapped firmware volumes.

 @param This               Indicates the EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL instance.

 @param Address            Pointer to a caller-allocated
                           EFI_PHYSICAL_ADDRESS that, on successful
                           return from GetPhysicalAddress(), contains the
                           base address of the firmware volume.

 @retval EFI_SUCCESS       The firmware volume base address was returned.

 @retval EFI_NOT_SUPPORTED The firmware volume is not memory mapped.

 **/
EFI_STATUS
EFIAPI
FvbGetPhysicalAddress(
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  OUT       EFI_PHYSICAL_ADDRESS                 *Address
  )
{
  NOR_FLASH_INSTANCE *Instance;

  Instance = INSTANCE_FROM_FVB_THIS(This);

  DEBUG ((DEBUG_BLKIO, "FvbGetPhysicalAddress(BaseAddress=0x%08x)\n", Instance->BaseAddress));

  ASSERT(Address != NULL);

  *Address = Instance->BaseAddress;
  return EFI_SUCCESS;
}

/**
 The GetBlockSize() function retrieves the size of the requested
 block. It also returns the number of additional blocks with
 the identical size. The GetBlockSize() function is used to
 retrieve the block map (see EFI_FIRMWARE_VOLUME_HEADER).


 @param This                     Indicates the EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL instance.

 @param Lba                      Indicates the block for which to return the size.

 @param BlockSize                Pointer to a caller-allocated UINTN in which
                                 the size of the block is returned.

 @param NumberOfBlocks           Pointer to a caller-allocated UINTN in
                                 which the number of consecutive blocks,
                                 starting with Lba, is returned. All
                                 blocks in this range have a size of
                                 BlockSize.


 @retval EFI_SUCCESS             The firmware volume base address was returned.

 @retval EFI_INVALID_PARAMETER   The requested LBA is out of range.

 **/
EFI_STATUS
EFIAPI
FvbGetBlockSize(
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN        EFI_LBA                              Lba,
  OUT       UINTN                                *BlockSize,
  OUT       UINTN                                *NumberOfBlocks
  )
{
  EFI_STATUS Status;
  NOR_FLASH_INSTANCE *Instance;

  Instance = INSTANCE_FROM_FVB_THIS(This);

  DEBUG ((DEBUG_BLKIO, "FvbGetBlockSize(Lba=%ld, BlockSize=0x%x, LastBlock=%ld)\n", Lba, Instance->Media.BlockSize, Instance->Media.LastBlock));

  if (Lba > Instance->Media.LastBlock) {
    DEBUG ((EFI_D_ERROR, "FvbGetBlockSize: ERROR - Parameter LBA %ld is beyond the last Lba (%ld).\n", Lba, Instance->Media.LastBlock));
    Status = EFI_INVALID_PARAMETER;
  } else {
    // This is easy because in this platform each NorFlash device has equal sized blocks.
    *BlockSize = (UINTN) Instance->Media.BlockSize;
    *NumberOfBlocks = (UINTN) (Instance->Media.LastBlock - Lba + 1);

    DEBUG ((DEBUG_BLKIO, "FvbGetBlockSize: *BlockSize=0x%x, *NumberOfBlocks=0x%x.\n", *BlockSize, *NumberOfBlocks));

    Status = EFI_SUCCESS;
  }

  return Status;
}

/**
 Reads the specified number of bytes into a buffer from the specified block.

 The Read() function reads the requested number of bytes from the
 requested block and stores them in the provided buffer.
 Implementations should be mindful that the firmware volume
 might be in the ReadDisabled state. If it is in this state,
 the Read() function must return the status code
 EFI_ACCESS_DENIED without modifying the contents of the
 buffer. The Read() function must also prevent spanning block
 boundaries. If a read is requested that would span a block
 boundary, the read must read up to the boundary but not
 beyond. The output parameter NumBytes must be set to correctly
 indicate the number of bytes actually read. The caller must be
 aware that a read may be partially completed.

 @param This                 Indicates the EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL instance.

 @param Lba                  The starting logical block index from which to read.

 @param Offset               Offset into the block at which to begin reading.

 @param NumBytes             Pointer to a UINTN.
                             At entry, *NumBytes contains the total size of the buffer.
                             At exit, *NumBytes contains the total number of bytes read.

 @param Buffer               Pointer to a caller-allocated buffer that will be used
                             to hold the data that is read.

 @retval EFI_SUCCESS         The firmware volume was read successfully,  and contents are
                             in Buffer.

 @retval EFI_BAD_BUFFER_SIZE Read attempted across an LBA boundary.
                             On output, NumBytes contains the total number of bytes
                             returned in Buffer.

 @retval EFI_ACCESS_DENIED   The firmware volume is in the ReadDisabled state.

 @retval EFI_DEVICE_ERROR    The block device is not functioning correctly and could not be read.

 **/
EFI_STATUS
EFIAPI
FvbRead (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL   *This,
  IN        EFI_LBA                               Lba,
  IN        UINTN                                 Offset,
  IN OUT    UINTN                                 *NumBytes,
  IN OUT    UINT8                                 *Buffer
  )
{
  EFI_STATUS    Status;
  EFI_STATUS    TempStatus;
  UINTN         BlockSize;
  UINT8         *BlockBuffer;
  NOR_FLASH_INSTANCE *Instance;

  Instance = INSTANCE_FROM_FVB_THIS(This);

  DEBUG ((DEBUG_BLKIO, "FvbRead(Parameters: Lba=%ld, Offset=0x%x, *NumBytes=0x%x, Buffer @ 0x%08x)\n", Lba, Offset, *NumBytes, Buffer));

  if (!Instance->Initialized) {
    Instance->Initialize(Instance);
  }

  Status = EFI_SUCCESS;
  TempStatus = Status;

  // Cache the block size to avoid de-referencing pointers all the time
  BlockSize = Instance->Media.BlockSize;

  DEBUG ((DEBUG_BLKIO, "FvbRead: Check if (Offset=0x%x + NumBytes=0x%x) <= BlockSize=0x%x\n", Offset, *NumBytes, BlockSize ));

  // The read must not span block boundaries.
  // We need to check each variable individually because adding two large values together overflows.
  if ( ( Offset               >= BlockSize ) ||
       ( *NumBytes            >  BlockSize ) ||
       ( (Offset + *NumBytes) >  BlockSize )    ) {
    DEBUG ((EFI_D_ERROR, "FvbRead: ERROR - EFI_BAD_BUFFER_SIZE: (Offset=0x%x + NumBytes=0x%x) > BlockSize=0x%x\n", Offset, *NumBytes, BlockSize ));
    return EFI_BAD_BUFFER_SIZE;
  }

  // We must have some bytes to read
  if (*NumBytes == 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  // FixMe: Allow an arbitrary number of bytes to be read out, not just a multiple of block size.

  // Allocate runtime memory to read in the NOR Flash data. Variable Services are runtime.
  BlockBuffer = AllocateRuntimePool(BlockSize);

  // Check if the memory allocation was successful
  if( BlockBuffer == NULL ) {
    DEBUG ((EFI_D_ERROR, "FvbRead: ERROR - Could not allocate BlockBuffer @ 0x%08x.\n", BlockBuffer));
    return EFI_DEVICE_ERROR;
  }

  // Read NOR Flash data into shadow buffer
  TempStatus = NorFlashReadBlocks(Instance, Lba, BlockSize, BlockBuffer);
  if (EFI_ERROR (TempStatus)) {
    // Return one of the pre-approved error statuses
    Status = EFI_DEVICE_ERROR;
    goto FREE_MEMORY;
  }

  // Put the data at the appropriate location inside the buffer area
  DEBUG ((DEBUG_BLKIO, "FvbRead: CopyMem( Dst=0x%08x, Src=0x%08x, Size=0x%x ).\n", Buffer, BlockBuffer + Offset, *NumBytes));

  CopyMem(Buffer, BlockBuffer + Offset, *NumBytes);

FREE_MEMORY:
  FreePool(BlockBuffer);

  DEBUG ((DEBUG_BLKIO, "FvbRead - end\n"));
  return Status;
}

/**
 Writes the specified number of bytes from the input buffer to the block.

 The Write() function writes the specified number of bytes from
 the provided buffer to the specified block and offset. If the
 firmware volume is sticky write, the caller must ensure that
 all the bits of the specified range to write are in the
 EFI_FVB_ERASE_POLARITY state before calling the Write()
 function, or else the result will be unpredictable. This
 unpredictability arises because, for a sticky-write firmware
 volume, a write may negate a bit in the EFI_FVB_ERASE_POLARITY
 state but cannot flip it back again.  Before calling the
 Write() function,  it is recommended for the caller to first call
 the EraseBlocks() function to erase the specified block to
 write. A block erase cycle will transition bits from the
 (NOT)EFI_FVB_ERASE_POLARITY state back to the
 EFI_FVB_ERASE_POLARITY state. Implementations should be
 mindful that the firmware volume might be in the WriteDisabled
 state. If it is in this state, the Write() function must
 return the status code EFI_ACCESS_DENIED without modifying the
 contents of the firmware volume. The Write() function must
 also prevent spanning block boundaries. If a write is
 requested that spans a block boundary, the write must store up
 to the boundary but not beyond. The output parameter NumBytes
 must be set to correctly indicate the number of bytes actually
 written. The caller must be aware that a write may be
 partially completed. All writes, partial or otherwise, must be
 fully flushed to the hardware before the Write() service
 returns.

 @param This                 Indicates the EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL instance.

 @param Lba                  The starting logical block index to write to.

 @param Offset               Offset into the block at which to begin writing.

 @param NumBytes             The pointer to a UINTN.
                             At entry, *NumBytes contains the total size of the buffer.
                             At exit, *NumBytes contains the total number of bytes actually written.

 @param Buffer               The pointer to a caller-allocated buffer that contains the source for the write.

 @retval EFI_SUCCESS         The firmware volume was written successfully.

 @retval EFI_BAD_BUFFER_SIZE The write was attempted across an LBA boundary.
                             On output, NumBytes contains the total number of bytes
                             actually written.

 @retval EFI_ACCESS_DENIED   The firmware volume is in the WriteDisabled state.

 @retval EFI_DEVICE_ERROR    The block device is malfunctioning and could not be written.


 **/
EFI_STATUS
EFIAPI
FvbWrite (
  IN CONST  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL   *This,
  IN        EFI_LBA                               Lba,
  IN        UINTN                                 Offset,
  IN OUT    UINTN                                 *NumBytes,
  IN        UINT8                                 *Buffer
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  TempStatus;
  UINTN       BlockSize;
  UINT8       *BlockBuffer;
  NOR_FLASH_INSTANCE *Instance;

  Instance = INSTANCE_FROM_FVB_THIS(This);

  if (!Instance->Initialized) {
    Instance->Initialize(Instance);
  }

  DEBUG ((DEBUG_BLKIO, "FvbWrite(Parameters: Lba=%ld, Offset=0x%x, *NumBytes=0x%x, Buffer @ 0x%08x)\n", Lba, Offset, *NumBytes, Buffer));

  Status = EFI_SUCCESS;
  TempStatus = Status;

  // Detect WriteDisabled state
  if (Instance->Media.ReadOnly == TRUE) {
    DEBUG ((EFI_D_ERROR, "FvbWrite: ERROR - Can not write: Device is in WriteDisabled state.\n"));
    // It is in WriteDisabled state, return an error right away
    return EFI_ACCESS_DENIED;
  }

  // Cache the block size to avoid de-referencing pointers all the time
  BlockSize = Instance->Media.BlockSize;

  // The write must not span block boundaries.
  // We need to check each variable individually because adding two large values together overflows.
  if ( ( Offset               >= BlockSize ) ||
       ( *NumBytes            >  BlockSize ) ||
       ( (Offset + *NumBytes) >  BlockSize )    ) {
    DEBUG ((EFI_D_ERROR, "FvbWrite: ERROR - EFI_BAD_BUFFER_SIZE: (Offset=0x%x + NumBytes=0x%x) > BlockSize=0x%x\n", Offset, *NumBytes, BlockSize ));
    return EFI_BAD_BUFFER_SIZE;
  }

  // We must have some bytes to write
  if (*NumBytes == 0) {
    DEBUG ((EFI_D_ERROR, "FvbWrite: ERROR - EFI_BAD_BUFFER_SIZE: (Offset=0x%x + NumBytes=0x%x) > BlockSize=0x%x\n", Offset, *NumBytes, BlockSize ));
    return EFI_BAD_BUFFER_SIZE;
  }

  // Allocate runtime memory to read in the NOR Flash data.
  // Since the intention is to use this with Variable Services and since these are runtime,
  // allocate the memory from the runtime pool.
  BlockBuffer = AllocateRuntimePool(BlockSize);

  // Check we did get some memory
  if( BlockBuffer == NULL ) {
    DEBUG ((EFI_D_ERROR, "FvbWrite: ERROR - Can not allocate BlockBuffer @ 0x%08x.\n", BlockBuffer));
    return EFI_DEVICE_ERROR;
  }

  // Read NOR Flash data into shadow buffer
  TempStatus = NorFlashReadBlocks(Instance, Lba, BlockSize, BlockBuffer);
  if (EFI_ERROR (TempStatus)) {
    // Return one of the pre-approved error statuses
    Status = EFI_DEVICE_ERROR;
    goto FREE_MEMORY;
  }

  // Put the data at the appropriate location inside the buffer area
  CopyMem((BlockBuffer + Offset), Buffer, *NumBytes);

  // Write the modified buffer back to the NorFlash
  Status = NorFlashWriteBlocks(Instance, Lba, BlockSize, BlockBuffer);
  if (EFI_ERROR (TempStatus)) {
    // Return one of the pre-approved error statuses
    Status = EFI_DEVICE_ERROR;
    goto FREE_MEMORY;
  }

FREE_MEMORY:
  FreePool(BlockBuffer);
  return Status;
}

/**
 Erases and initialises a firmware volume block.

 The EraseBlocks() function erases one or more blocks as denoted
 by the variable argument list. The entire parameter list of
 blocks must be verified before erasing any blocks. If a block is
 requested that does not exist within the associated firmware
 volume (it has a larger index than the last block of the
 firmware volume), the EraseBlocks() function must return the
 status code EFI_INVALID_PARAMETER without modifying the contents
 of the firmware volume. Implementations should be mindful that
 the firmware volume might be in the WriteDisabled state. If it
 is in this state, the EraseBlocks() function must return the
 status code EFI_ACCESS_DENIED without modifying the contents of
 the firmware volume. All calls to EraseBlocks() must be fully
 flushed to the hardware before the EraseBlocks() service
 returns.

 @param This                     Indicates the EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL
 instance.

 @param ...                      The variable argument list is a list of tuples.
                                 Each tuple describes a range of LBAs to erase
                                 and consists of the following:
                                 - An EFI_LBA that indicates the starting LBA
                                 - A UINTN that indicates the number of blocks to erase.

                                 The list is terminated with an EFI_LBA_LIST_TERMINATOR.
                                 For example, the following indicates that two ranges of blocks
                                 (5-7 and 10-11) are to be erased:
                                 EraseBlocks (This, 5, 3, 10, 2, EFI_LBA_LIST_TERMINATOR);

 @retval EFI_SUCCESS             The erase request successfully completed.

 @retval EFI_ACCESS_DENIED       The firmware volume is in the WriteDisabled state.

 @retval EFI_DEVICE_ERROR        The block device is not functioning correctly and could not be written.
                                 The firmware device may have been partially erased.

 @retval EFI_INVALID_PARAMETER   One or more of the LBAs listed in the variable argument list do
                                 not exist in the firmware volume.

 **/
EFI_STATUS
EFIAPI
FvbEraseBlocks (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL *This,
  ...
  )
{
  EFI_STATUS  Status;
  VA_LIST     args;
  UINTN       BlockAddress; // Physical address of Lba to erase
  EFI_LBA     StartingLba; // Lba from which we start erasing
  UINTN       NumOfLba; // Number of Lba blocks to erase
  NOR_FLASH_INSTANCE *Instance;

  Instance = INSTANCE_FROM_FVB_THIS(This);

  DEBUG ((DEBUG_BLKIO, "FvbEraseBlocks()\n"));

  Status = EFI_SUCCESS;

  // Detect WriteDisabled state
  if (Instance->Media.ReadOnly == TRUE) {
    // Firmware volume is in WriteDisabled state
    DEBUG ((EFI_D_ERROR, "FvbEraseBlocks: ERROR - Device is in WriteDisabled state.\n"));
    return EFI_ACCESS_DENIED;
  }

  // Before erasing, check the entire list of parameters to ensure all specified blocks are valid

  VA_START (args, This);

  do {

    // Get the Lba from which we start erasing
    StartingLba = VA_ARG (args, EFI_LBA);

    // Have we reached the end of the list?
    if (StartingLba == EFI_LBA_LIST_TERMINATOR) {
      //Exit the while loop
      break;
    }

    // How many Lba blocks are we requested to erase?
    NumOfLba = VA_ARG (args, UINT32);

    // All blocks must be within range
    DEBUG ((DEBUG_BLKIO, "FvbEraseBlocks: Check if: ( StartingLba=%ld + NumOfLba=%d - 1 ) > LastBlock=%ld.\n", StartingLba, NumOfLba, Instance->Media.LastBlock));
    if ((NumOfLba == 0) || ((StartingLba + NumOfLba - 1) > Instance->Media.LastBlock)) {
      VA_END (args);
      DEBUG ((DEBUG_BLKIO, "FvbEraseBlocks: Check if: ( StartingLba=%ld + NumOfLba=%d - 1 ) > LastBlock=%ld.\n", StartingLba, NumOfLba, Instance->Media.LastBlock));
      DEBUG ((EFI_D_ERROR, "FvbEraseBlocks: ERROR - Lba range goes past the last Lba.\n"));
      Status = EFI_INVALID_PARAMETER;
      goto EXIT;
    }

  } while (TRUE);

  VA_END (args);

  // To get here, all must be ok, so start erasing

  VA_START (args, This);

  do {

    // Get the Lba from which we start erasing
    StartingLba = VA_ARG (args, EFI_LBA);

    // Have we reached the end of the list?
    if (StartingLba == EFI_LBA_LIST_TERMINATOR) {
      // Exit the while loop
      break;
    }

    // How many Lba blocks are we requested to erase?
    NumOfLba = VA_ARG (args, UINT32);

    // Go through each one and erase it
    while (NumOfLba > 0) {

      // Get the physical address of Lba to erase
      BlockAddress = GET_NOR_BLOCK_ADDRESS (
          Instance->BaseAddress,
          StartingLba,
          Instance->Media.BlockSize
      );

      // Erase it
      DEBUG ((DEBUG_BLKIO, "FvbEraseBlocks: Erasing Lba=%ld @ 0x%08x.\n", StartingLba, BlockAddress));
      Status = NorFlashUnlockAndEraseSingleBlock (BlockAddress);
      if (EFI_ERROR(Status)) {
        VA_END (args);
        Status = EFI_DEVICE_ERROR;
        goto EXIT;
      }

      // Move to the next Lba
      StartingLba++;
      NumOfLba--;
    }

  } while (TRUE);

  VA_END (args);

EXIT:
  return Status;
}

EFI_STATUS
EFIAPI
NorFlashFvbInitialize (
  IN NOR_FLASH_INSTANCE* Instance
  ) {
  EFI_STATUS Status;

  DEBUG((DEBUG_BLKIO,"NorFlashFvbInitialize\n"));

  Status = NorFlashBlkIoInitialize(Instance);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR,"NorFlashFvbInitialize: ERROR - Failed to initialize FVB\n"));
    return Status;
  }
  Instance->Initialized = TRUE;

  // Determine if there is a valid header at the beginning of the NorFlash
  Status = ValidateFvHeader (Instance);
  if (EFI_ERROR(Status)) {
    // There is no valid header, so time to install one.
    DEBUG((EFI_D_ERROR,"NorFlashFvbInitialize: ERROR - The FVB Header is not valid. Installing a correct one for this volume.\n"));

    // Erase all the NorFlash that is reserved for variable storage
    Status = FvbEraseBlocks ( &Instance->FvbProtocol, (EFI_LBA)0, (UINT32)(Instance->Media.LastBlock + 1), EFI_LBA_LIST_TERMINATOR );
    if (EFI_ERROR(Status)) {
      return Status;
    }

    // Install all appropriate headers
    InitializeFvAndVariableStoreHeaders ( Instance );
    if (EFI_ERROR(Status)) {
      return Status;
    }
  }

  return Status;
}