/** @file
  Implement the IP4 driver support for the socket layer.

  Copyright (c) 2011, Intel Corporation
  All rights reserved. This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.


  \section Ip4ReceiveEngine IPv4 Receive Engine

  The receive engine is started by calling ::EslIp4RxStart when the
  ::ESL_PORT structure is configured and stopped when ::EslSocketPortCloseTxDone
  calls the IPv4 configure operation to reset the port.  The receive engine
  consists of a single receive buffer that is posted to the IPv4 driver.

  Upon receive completion, ::EslIp4RxComplete posts the IPv4 buffer to the
  ESL_SOCKET::pRxPacketListTail.  To minimize the number of buffer copies,
  the ::EslIp4RxComplete routine queues the IP4 driver's buffer to a list
  of datagrams waiting to be received.  The socket driver holds on to the
  buffers from the IPv4 driver until the application layer requests
  the data or the socket is closed.

  When the application wants to receive data it indirectly calls
  ::EslIp4Receive to remove data from the data queue.  This routine
  removes the next available datagram from ESL_SOCKET::pRxPacketListHead
  and copies the data from the IPv4 driver's buffer into the
  application's buffer.  The IPv4 driver's buffer is then returned.

  During socket layer shutdown, ::EslIp4RxCancel is called by ::EslSocketShutdown
  to cancel the pending receive operations.

  Receive flow control is applied when the socket is created, since no receive
  operation is pending to the IPv4 driver.  The flow control gets released
  when the port is configured.  Flow control remains in the released state,
  ::EslIp4RxComplete calls ::EslIp4RxStart until the maximum buffer space
  is consumed.  By not calling EslIp4RxStart, EslIp4RxComplete applies flow
  control.  Flow control is eventually released when the buffer space drops
  below the maximum amount and EslIp4Receive calls EslIp4RxStart.

**/

#include "Socket.h"

/**
  Interface between the socket layer and the network specific
  code that supports SOCK_RAW sockets over IPv4.
**/
CONST ESL_PROTOCOL_API cEslIp4Api = {
  IPPROTO_IP,
  NULL,   //  Accept
  EslIp4Bind,
  EslIp4Connect,
  NULL,   //  ConnectPoll
  EslIp4GetLocalAddress,
  EslIp4GetRemoteAddress,
  EslIp4SocketIsConfigured,
  NULL,   //  Listen
  EslIp4OptionGet,
  EslIp4OptionSet,
  EslIp4PortClose,
  EslIp4PortClosePacketFree,
  EslIp4PortCloseRxStop,
  TRUE,
  EslIp4Receive,
  EslIp4RxCancel,
  EslIp4TxBuffer
};


/**
  Bind a name to a socket.

  This routine connects a name (IPv4 address) to the IPv4 stack
  on the local machine.

  This routine is called by ::EslSocketBind to handle the IPv4 specific
  protocol bind operations for SOCK_RAW sockets.

  The configure call to the IP4 driver occurs on the first poll, recv, recvfrom,
  send or sentto call.  Until then, all changes are made in the local IP context
  structure.

  @param [in] pSocket   Address of an ::ESL_SOCKET structure.

  @param [in] pSockAddr Address of a sockaddr structure that contains the
                        connection point on the local machine.  An IPv4 address
                        of INADDR_ANY specifies that the connection is made to
                        all of the network stacks on the platform.  Specifying a
                        specific IPv4 address restricts the connection to the
                        network stack supporting that address.

  @param [in] SockAddrLength  Specifies the length in bytes of the sockaddr structure.

  @retval EFI_SUCCESS - Socket successfully created

 **/
EFI_STATUS
EslIp4Bind (
  IN ESL_SOCKET * pSocket,
  IN const struct sockaddr * pSockAddr,
  IN socklen_t SockAddrLength
  )
{
  EFI_HANDLE ChildHandle;
  CONST struct sockaddr_in * pIp4Address;
  EFI_SERVICE_BINDING_PROTOCOL * pServiceBinding;
  ESL_LAYER * pLayer;
  ESL_PORT * pPort;
  ESL_SERVICE * pService;
  EFI_STATUS Status;
  EFI_STATUS TempStatus;
  
  DBG_ENTER ( );
  
  //
  //  Verify the socket layer synchronization
  //
  VERIFY_TPL ( TPL_SOCKETS );
  
  //
  //  Assume success
  //
  pSocket->errno = 0;
  Status = EFI_SUCCESS;
  
  //
  //  Validate the address length
  //
  pIp4Address = (CONST struct sockaddr_in *) pSockAddr;
  if ( SockAddrLength >= ( sizeof ( *pIp4Address )
                           - sizeof ( pIp4Address->sin_zero ))) {

    //
    //  Walk the list of services
    //
    pLayer = &mEslLayer;
    pService = pLayer->pIp4List;
    while ( NULL != pService ) {

      //
      //  Create the IP port
      //
      pServiceBinding = pService->pServiceBinding;
      ChildHandle = NULL;
      Status = pServiceBinding->CreateChild ( pServiceBinding,
                                              &ChildHandle );
      if ( !EFI_ERROR ( Status )) {
        DEBUG (( DEBUG_BIND | DEBUG_POOL,
                  "0x%08x: Ip4 port handle created\r\n",
                  ChildHandle ));
  
        //
        //  Open the port
        //
        Status = EslIp4PortAllocate ( pSocket,
                                      pService,
                                      ChildHandle,
                                      (UINT8 *) &pIp4Address->sin_addr.s_addr,
                                      DEBUG_BIND,
                                      &pPort );
      }
      else {
        DEBUG (( DEBUG_BIND | DEBUG_POOL,
                  "ERROR - Failed to open Ip4 port handle, Status: %r\r\n",
                  Status ));
        ChildHandle = NULL;
      }
  
      //
      //  Close the port if necessary
      //
      if (( EFI_ERROR ( Status )) && ( NULL != ChildHandle )) {
        TempStatus = pServiceBinding->DestroyChild ( pServiceBinding,
                                                     ChildHandle );
        if ( !EFI_ERROR ( TempStatus )) {
          DEBUG (( DEBUG_BIND | DEBUG_POOL,
                    "0x%08x: Ip4 port handle destroyed\r\n",
                    ChildHandle ));
        }
        else {
          DEBUG (( DEBUG_ERROR | DEBUG_BIND | DEBUG_POOL,
                    "ERROR - Failed to destroy the Ip4 port handle 0x%08x, Status: %r\r\n",
                    ChildHandle,
                    TempStatus ));
          ASSERT ( EFI_SUCCESS == TempStatus );
        }
      }
  
      //
      //  Set the next service
      //
      pService = pService->pNext;
    }
  
    //
    //  Verify that at least one network connection was found
    //
    if ( NULL == pSocket->pPortList ) {
      DEBUG (( DEBUG_BIND | DEBUG_POOL | DEBUG_INIT,
                "Socket address %d.%d.%d.%d (0x%08x) is not available!\r\n",
                ( pIp4Address->sin_addr.s_addr >> 24 ) & 0xff,
                ( pIp4Address->sin_addr.s_addr >> 16 ) & 0xff,
                ( pIp4Address->sin_addr.s_addr >> 8 ) & 0xff,
                pIp4Address->sin_addr.s_addr & 0xff,
                pIp4Address->sin_addr.s_addr ));
      pSocket->errno = EADDRNOTAVAIL;
      Status = EFI_INVALID_PARAMETER;
    }
  }
  else {
    DEBUG (( DEBUG_BIND,
              "ERROR - Invalid Ip4 address length: %d\r\n",
              SockAddrLength ));
    Status = EFI_INVALID_PARAMETER;
    pSocket->errno = EINVAL;
  }
  
  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Set the default remote system address.

  This routine sets the default remote address for a SOCK_RAW
  socket using the IPv4 network layer.

  This routine is called by ::EslSocketConnect to initiate the IPv4
  network specific connect operations.  The connection processing is
  limited to setting the default remote network address.

  @param [in] pSocket         Address of an ::ESL_SOCKET structure.

  @param [in] pSockAddr       Network address of the remote system.
    
  @param [in] SockAddrLength  Length in bytes of the network address.
  
  @retval EFI_SUCCESS   The connection was successfully established.
  @retval EFI_NOT_READY The connection is in progress, call this routine again.
  @retval Others        The connection attempt failed.

 **/
EFI_STATUS
EslIp4Connect (
  IN ESL_SOCKET * pSocket,
  IN const struct sockaddr * pSockAddr,
  IN socklen_t SockAddrLength
  )
{
  struct sockaddr_in LocalAddress;
  ESL_PORT * pPort;
  struct sockaddr_in * pRemoteAddress;
  ESL_IP4_CONTEXT * pIp4;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Assume failure
  //
  Status = EFI_NETWORK_UNREACHABLE;
  pSocket->errno = ENETUNREACH;

  //
  //  Get the address
  //
  pRemoteAddress = (struct sockaddr_in *)pSockAddr;

  //
  //  Validate the address length
  //
  if ( SockAddrLength >= ( sizeof ( *pRemoteAddress )
                           - sizeof ( pRemoteAddress->sin_zero ))) {
    //
    //  Determine if BIND was already called
    //
    if ( NULL == pSocket->pPortList ) {
      //
      //  Allow any local port
      //
      ZeroMem ( &LocalAddress, sizeof ( LocalAddress ));
      LocalAddress.sin_len = sizeof ( LocalAddress );
      LocalAddress.sin_family = AF_INET;
      Status = EslSocketBind ( &pSocket->SocketProtocol,
                               (struct sockaddr *)&LocalAddress,
                               LocalAddress.sin_len,
                               &pSocket->errno );
    }

    //
    //  Walk the list of ports
    //
    pPort = pSocket->pPortList;
    while ( NULL != pPort ) {
      //
      //  Set the remote address
      //
      pIp4 = &pPort->Context.Ip4;
      pIp4->DestinationAddress.Addr[0] = (UINT8)( pRemoteAddress->sin_addr.s_addr );
      pIp4->DestinationAddress.Addr[1] = (UINT8)( pRemoteAddress->sin_addr.s_addr >> 8 );
      pIp4->DestinationAddress.Addr[2] = (UINT8)( pRemoteAddress->sin_addr.s_addr >> 16 );
      pIp4->DestinationAddress.Addr[3] = (UINT8)( pRemoteAddress->sin_addr.s_addr >> 24 );

      //
      //  At least one path exists
      //
      Status = EFI_SUCCESS;
      pSocket->errno = 0;

      //
      //  Set the next port
      //
      pPort = pPort->pLinkSocket;
    }
  }
  else {
    DEBUG (( DEBUG_CONNECT,
              "ERROR - Invalid IP4 address length: %d\r\n",
              SockAddrLength ));
    Status = EFI_INVALID_PARAMETER;
    pSocket->errno = EINVAL;
  }

  //
  //  Return the connect status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Get the local socket address

  This routine returns the IPv4 address associated with the local
  socket.

  This routine is called by ::EslSocketGetLocalAddress to determine the
  network address for the SOCK_RAW socket.

  @param [in] pSocket             Address of an ::ESL_SOCKET structure.

  @param [out] pAddress           Network address to receive the local system address

  @param [in,out] pAddressLength  Length of the local network address structure

  @retval EFI_SUCCESS - Address available
  @retval Other - Failed to get the address

**/
EFI_STATUS
EslIp4GetLocalAddress (
  IN ESL_SOCKET * pSocket,
  OUT struct sockaddr * pAddress,
  IN OUT socklen_t * pAddressLength
  )
{
  socklen_t LengthInBytes;
  ESL_PORT * pPort;
  struct sockaddr_in * pLocalAddress;
  ESL_IP4_CONTEXT * pIp4;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Verify the socket layer synchronization
  //
  VERIFY_TPL ( TPL_SOCKETS );

  //
  //  Verify that there is just a single connection
  //
  pPort = pSocket->pPortList;
  if (( NULL != pPort ) && ( NULL == pPort->pLinkSocket )) {
    //
    //  Verify the address length
    //
    LengthInBytes = sizeof ( struct sockaddr_in );
    if ( LengthInBytes <= *pAddressLength ) {
      //
      //  Return the local address
      //
      pIp4 = &pPort->Context.Ip4;
      pLocalAddress = (struct sockaddr_in *)pAddress;
      ZeroMem ( pLocalAddress, LengthInBytes );
      pLocalAddress->sin_family = AF_INET;
      pLocalAddress->sin_len = (uint8_t)LengthInBytes;
      CopyMem ( &pLocalAddress->sin_addr,
                &pIp4->ModeData.ConfigData.StationAddress.Addr[0],
                sizeof ( pLocalAddress->sin_addr ));
      pSocket->errno = 0;
      Status = EFI_SUCCESS;
    }
    else {
      pSocket->errno = EINVAL;
      Status = EFI_INVALID_PARAMETER;
    }
  }
  else {
    pSocket->errno = ENOTCONN;
    Status = EFI_NOT_STARTED;
  }
  
  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Get the remote socket address

  This routine returns the address of the remote connection point
  associated with the SOCK_RAW socket.

  This routine is called by ::EslSocketGetPeerAddress to detemine
  the IPv4 address associated with the network adapter.

  @param [in] pSocket             Address of an ::ESL_SOCKET structure.

  @param [out] pAddress           Network address to receive the remote system address

  @param [in,out] pAddressLength  Length of the remote network address structure

  @retval EFI_SUCCESS - Address available
  @retval Other - Failed to get the address

**/
EFI_STATUS
EslIp4GetRemoteAddress (
  IN ESL_SOCKET * pSocket,
  OUT struct sockaddr * pAddress,
  IN OUT socklen_t * pAddressLength
  )
{
  socklen_t LengthInBytes;
  ESL_PORT * pPort;
  struct sockaddr_in * pRemoteAddress;
  ESL_IP4_CONTEXT * pIp4;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Verify the socket layer synchronization
  //
  VERIFY_TPL ( TPL_SOCKETS );

  //
  //  Verify that there is just a single connection
  //
  pPort = pSocket->pPortList;
  if (( NULL != pPort ) && ( NULL == pPort->pLinkSocket )) {
    //
    //  Verify the address length
    //
    LengthInBytes = sizeof ( struct sockaddr_in );
    if ( LengthInBytes <= *pAddressLength ) {
      //
      //  Return the local address
      //
      pIp4 = &pPort->Context.Ip4;
      pRemoteAddress = (struct sockaddr_in *)pAddress;
      ZeroMem ( pRemoteAddress, LengthInBytes );
      pRemoteAddress->sin_family = AF_INET;
      pRemoteAddress->sin_len = (uint8_t)LengthInBytes;
      CopyMem ( &pRemoteAddress->sin_addr,
                &pIp4->DestinationAddress.Addr[0],
                sizeof ( pRemoteAddress->sin_addr ));
      pSocket->errno = 0;
      Status = EFI_SUCCESS;
    }
    else {
      pSocket->errno = EINVAL;
      Status = EFI_INVALID_PARAMETER;
    }
  }
  else {
    pSocket->errno = ENOTCONN;
    Status = EFI_NOT_STARTED;
  }
  
  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Initialize the IP4 service.

  This routine initializes the IP4 service which is used by the
  sockets layer to support SOCK_RAW sockets.

  This routine is called by ::EslServiceConnect after initializing an
  ::ESL_SERVICE structure for an adapter running IPv4.

  @param [in] pService        Address of an ::ESL_SERVICE structure

  @retval EFI_SUCCESS         The service was properly initialized
  @retval other               A failure occurred during the service initialization

**/
EFI_STATUS
EFIAPI
EslIp4Initialize (
  IN ESL_SERVICE * pService
  )
{
  ESL_LAYER * pLayer;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Connect this service to the service list
  //
  pLayer = &mEslLayer;
  pService->pNext = pLayer->pIp4List;
  pLayer->pIp4List = pService;

  //
  //  Assume the list is empty
  //
  Status = EFI_SUCCESS;

  //
  //  Return the initialization status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Get the option value

  This routine handles the IPv4 level options.

  The ::EslSocketOptionGet routine calls this routine to retrieve
  the IPv4 options one at a time by name.

  @param [in] pSocket           Address of an ::ESL_SOCKET structure
  @param [in] level             Option protocol level
  @param [in] OptionName        Name of the option
  @param [out] ppOptionData     Buffer to receive address of option value
  @param [out] pOptionLength    Buffer to receive the option length

  @retval EFI_SUCCESS - Socket data successfully received

 **/
EFI_STATUS
EslIp4OptionGet (
  IN ESL_SOCKET * pSocket,
  IN int level,
  IN int OptionName,
  OUT CONST void ** __restrict ppOptionData,
  OUT socklen_t * __restrict pOptionLength
  )
{
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Assume success
  //
  pSocket->errno = 0;
  Status = EFI_SUCCESS;

  //
  //  Attempt to get the option
  //
  switch ( level ) {
  default:
    //
    //  Protocol level not supported
    //
    pSocket->errno = ENOTSUP;
    Status = EFI_UNSUPPORTED;
    break;

  case IPPROTO_IP:
    switch ( OptionName ) {
    default:
      //
      //  Option not supported
      //
      pSocket->errno = ENOTSUP;
      Status = EFI_UNSUPPORTED;
      break;

    case IP_HDRINCL:
      *ppOptionData = (void *)pSocket->bIncludeHeader;
      *pOptionLength = sizeof ( pSocket->bIncludeHeader );
      break;
    }
  }

  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Set the option value

  This routine handles the IPv4 level options.

  The ::EslSocketOptionSet routine calls this routine to adjust
  the IPv4 options one at a time by name.

  @param [in] pSocket         Address of an ::ESL_SOCKET structure
  @param [in] level           Option protocol level
  @param [in] OptionName      Name of the option
  @param [in] pOptionValue    Buffer containing the option value
  @param [in] OptionLength    Length of the buffer in bytes

  @retval EFI_SUCCESS - Option successfully set

 **/
EFI_STATUS
EslIp4OptionSet (
  IN ESL_SOCKET * pSocket,
  IN int level,
  IN int OptionName,
  IN CONST void * pOptionValue,
  IN socklen_t OptionLength
  )
{
  BOOLEAN bTrueFalse;
  socklen_t LengthInBytes;
  UINT8 * pOptionData;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Assume failure
  //
  pSocket->errno = EINVAL;
  Status = EFI_INVALID_PARAMETER;

  //
  //  Determine if the option protocol matches
  //
  LengthInBytes = 0;
  pOptionData = NULL;
  switch ( level ) {
  default:
    //
    //  Protocol level not supported
    //
    DEBUG (( DEBUG_INFO | DEBUG_OPTION, "ERROR - Invalid option level\r\n" ));
    pSocket->errno = ENOTSUP;
    Status = EFI_UNSUPPORTED;
    break;

  case IPPROTO_IP:
    switch ( OptionName ) {
    default:
      //
      //  Protocol level not supported
      //
      DEBUG (( DEBUG_INFO | DEBUG_OPTION, "ERROR - Invalid protocol option\r\n" ));
      pSocket->errno = ENOTSUP;
      Status = EFI_UNSUPPORTED;
      break;

    case IP_HDRINCL:

      //
      //  Validate the option length
      //
      if ( sizeof ( UINT32 ) == OptionLength ) {
        //
        //  Restrict the input to TRUE or FALSE
        //
        bTrueFalse = TRUE;
        if ( 0 == *(UINT32 *)pOptionValue ) {
          bTrueFalse = FALSE;
        }
        pOptionValue = &bTrueFalse;

        //
        //  Set the option value
        //
        pOptionData = (UINT8 *)&pSocket->bIncludeHeader;
        LengthInBytes = sizeof ( pSocket->bIncludeHeader );
      }
      break;
      
    }
    break;
  }

  //
  //  Validate the option length
  //
  if ( LengthInBytes <= OptionLength ) {
    //
    //  Set the option value
    //
    if ( NULL != pOptionData ) {
      CopyMem ( pOptionData, pOptionValue, LengthInBytes );
      pSocket->errno = 0;
      Status = EFI_SUCCESS;
    }
  }
  
  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}



/**
  Allocate and initialize an ::ESL_PORT structure.

  This routine initializes an ::ESL_PORT structure for use by the
  socket.

  This support routine is called by ::EslIp4Bind to connect the
  socket with the underlying network adapter running the IPv4
  protocol.

  @param [in] pSocket     Address of an ::ESL_SOCKET structure.
  @param [in] pService    Address of an ::ESL_SERVICE structure.
  @param [in] ChildHandle Ip4 child handle
  @param [in] pIpAddress  Buffer containing IP4 network address of the local host
  @param [in] DebugFlags  Flags for debug messages
  @param [out] ppPort     Buffer to receive new ESL_PORT structure address

  @retval EFI_SUCCESS - Socket successfully created

 **/
EFI_STATUS
EslIp4PortAllocate (
  IN ESL_SOCKET * pSocket,
  IN ESL_SERVICE * pService,
  IN EFI_HANDLE ChildHandle,
  IN CONST UINT8 * pIpAddress,
  IN UINTN DebugFlags,
  OUT ESL_PORT ** ppPort
  )
{
  UINTN LengthInBytes;
  UINT8 * pBuffer;
  EFI_IP4_CONFIG_DATA * pConfig;
  ESL_IO_MGMT * pIo;
  ESL_IP4_CONTEXT * pIp4;
  ESL_LAYER * pLayer;
  ESL_PORT * pPort;
  CONST ESL_SOCKET_BINDING * pSocketBinding;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Use for/break instead of goto
  for ( ; ; ) {
    //
    //  Allocate a port structure
    //
    pLayer = &mEslLayer;
    LengthInBytes = sizeof ( *pPort )
                  + ESL_STRUCTURE_ALIGNMENT_BYTES
                  + ( pService->pSocketBinding->TxIoNormal
                      * sizeof ( ESL_IO_MGMT ));
    Status = gBS->AllocatePool ( EfiRuntimeServicesData,
                                 LengthInBytes,
                                 (VOID **)&pPort );
    if ( EFI_ERROR ( Status )) {
      DEBUG (( DEBUG_ERROR | DebugFlags | DEBUG_POOL | DEBUG_INIT,
                "ERROR - Failed to allocate the port structure, Status: %r\r\n",
                Status ));
      pSocket->errno = ENOMEM;
      pPort = NULL;
      break;
    }
    DEBUG (( DebugFlags | DEBUG_POOL | DEBUG_INIT,
              "0x%08x: Allocate pPort, %d bytes\r\n",
              pPort,
              LengthInBytes ));

    //
    //  Initialize the port
    //
    ZeroMem ( pPort, LengthInBytes );
    pPort->Signature = PORT_SIGNATURE;
    pPort->pService = pService;
    pPort->pSocket = pSocket;
    pPort->DebugFlags = DebugFlags;
    pSocket->TxPacketOffset = OFFSET_OF ( ESL_PACKET, Op.Ip4Tx.TxData );
    pSocket->TxTokenEventOffset = OFFSET_OF ( ESL_IO_MGMT, Token.Ip4Tx.Event );
    pSocket->TxTokenOffset = OFFSET_OF ( EFI_IP4_COMPLETION_TOKEN, Packet.TxData );
    pBuffer = (UINT8 *)&pPort[ 1 ];
    pBuffer = &pBuffer[ ESL_STRUCTURE_ALIGNMENT_BYTES ];
    pBuffer = (UINT8 *)( ESL_STRUCTURE_ALIGNMENT_MASK & (UINTN)pBuffer );
    pIo = (ESL_IO_MGMT *)pBuffer;

    //
    //  Allocate the receive event
    //
    pIp4 = &pPort->Context.Ip4;
    Status = gBS->CreateEvent (  EVT_NOTIFY_SIGNAL,
                                 TPL_SOCKETS,
                                 (EFI_EVENT_NOTIFY)EslIp4RxComplete,
                                 pPort,
                                 &pIp4->RxToken.Event);
    if ( EFI_ERROR ( Status )) {
      DEBUG (( DEBUG_ERROR | DebugFlags,
                "ERROR - Failed to create the receive event, Status: %r\r\n",
                Status ));
      pSocket->errno = ENOMEM;
      break;
    }
    DEBUG (( DEBUG_RX | DEBUG_POOL,
              "0x%08x: Created receive event\r\n",
              pIp4->RxToken.Event ));

    //
    //  Allocate the transmit events
    //
    Status = EslSocketIoInit ( pPort,
                               &pIo,
                               pService->pSocketBinding->TxIoNormal,
                               &pPort->pTxFree,
                               DebugFlags | DEBUG_POOL,
                               "transmit",
                               OFFSET_OF ( ESL_IO_MGMT, Token.Ip4Tx.Event ),
                               EslIp4TxComplete );
    if ( EFI_ERROR ( Status )) {
      break;
    }

    //
    //  Open the port protocol
    //
    pSocketBinding = pService->pSocketBinding;
    Status = gBS->OpenProtocol (
                    ChildHandle,
                    pSocketBinding->pNetworkProtocolGuid,
                    &pPort->pProtocol.v,
                    pLayer->ImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL );
    if ( EFI_ERROR ( Status )) {
      DEBUG (( DEBUG_ERROR | DebugFlags,
                "ERROR - Failed to open gEfiIp4ProtocolGuid on controller 0x%08x\r\n",
                pPort->Handle ));
      pSocket->errno = EEXIST;
      break;
    }
    DEBUG (( DebugFlags,
              "0x%08x: gEfiIp4ProtocolGuid opened on controller 0x%08x\r\n",
              pPort->pProtocol.v,
              ChildHandle ));

    //
    //  Save the transmit address
    //
    pPort->pfnTxStart = (PFN_NET_TX_START)pPort->pProtocol.IPv4->Transmit;

    //
    //  Set the port address
    //
    pPort->Handle = ChildHandle;
    pConfig = &pPort->Context.Ip4.ModeData.ConfigData;
    pConfig->DefaultProtocol = (UINT8)pSocket->Protocol;
    if (( 0 == pIpAddress[0])
      && ( 0 == pIpAddress[1])
      && ( 0 == pIpAddress[2])
      && ( 0 == pIpAddress[3])) {
      pConfig->UseDefaultAddress = TRUE;
      DEBUG (( DebugFlags,
                "0x%08x: Port using default IP address\r\n",
                pPort ));
    }
    else {
      pConfig->StationAddress.Addr[0] = pIpAddress[0];
      pConfig->StationAddress.Addr[1] = pIpAddress[1];
      pConfig->StationAddress.Addr[2] = pIpAddress[2];
      pConfig->StationAddress.Addr[3] = pIpAddress[3];
      pConfig->SubnetMask.Addr[0] = 0xff;
      pConfig->SubnetMask.Addr[1] = 0xff;
      pConfig->SubnetMask.Addr[2] = 0xff;
      pConfig->SubnetMask.Addr[3] = 0xff;
      DEBUG (( DebugFlags,
                "0x%08x: Port using IP address: %d.%d.%d.%d\r\n",
                pPort,
                pConfig->StationAddress.Addr[0],
                pConfig->StationAddress.Addr[1],
                pConfig->StationAddress.Addr[2],
                pConfig->StationAddress.Addr[3]));
    }
    pConfig->AcceptAnyProtocol = (BOOLEAN)( 0 == pConfig->UseDefaultAddress);
    pConfig->AcceptIcmpErrors = FALSE;
    pConfig->AcceptBroadcast = FALSE;
    pConfig->AcceptPromiscuous = FALSE;
    pConfig->TypeOfService = 0;
    pConfig->TimeToLive = 255;
    pConfig->DoNotFragment = FALSE;
    pConfig->RawData = FALSE;
    pConfig->ReceiveTimeout = 0;
    pConfig->TransmitTimeout = 0;

    //
    //  Verify the socket layer synchronization
    //
    VERIFY_TPL ( TPL_SOCKETS );

    //
    //  Add this port to the socket
    //
    pPort->pLinkSocket = pSocket->pPortList;
    pSocket->pPortList = pPort;
    DEBUG (( DebugFlags,
              "0x%08x: Socket adding port: 0x%08x\r\n",
              pSocket,
              pPort ));

    //
    //  Add this port to the service
    //
    pPort->pLinkService = pService->pPortList;
    pService->pPortList = pPort;

    //
    //  Return the port
    //
    *ppPort = pPort;
    break;
  }

  //
  //  Clean up after the error if necessary
  //
  if (( EFI_ERROR ( Status )) && ( NULL != pPort )) {
    //
    //  Close the port
    //
    EslSocketPortClose ( pPort );
  }
  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Close an IP4 port.

  This routine releases the resources allocated by
  ::EslIp4PortAllocate.

  This routine is called by ::EslSocketPortClose.
  See the \ref PortCloseStateMachine section.

  @param [in] pPort       Address of an ::ESL_PORT structure.

  @retval EFI_SUCCESS     The port is closed
  @retval other           Port close error

**/
EFI_STATUS
EslIp4PortClose (
  IN ESL_PORT * pPort
  )
{
  UINTN DebugFlags;
  ESL_IP4_CONTEXT * pIp4;
  EFI_STATUS Status;
  
  DBG_ENTER ( );

  //
  //  Assume success
  //
  Status = EFI_SUCCESS;
  DebugFlags = pPort->DebugFlags;
  pIp4 = &pPort->Context.Ip4;

  //
  //  Done with the receive event
  //
  if ( NULL != pIp4->RxToken.Event ) {
    Status = gBS->CloseEvent ( pIp4->RxToken.Event );
    if ( !EFI_ERROR ( Status )) {
      DEBUG (( DebugFlags | DEBUG_POOL,
                "0x%08x: Closed receive event\r\n",
                pIp4->RxToken.Event ));
    }
    else {
      DEBUG (( DEBUG_ERROR | DebugFlags,
                "ERROR - Failed to close the receive event, Status: %r\r\n",
                Status ));
      ASSERT ( EFI_SUCCESS == Status );
    }
  }

  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Free a receive packet

  This routine performs the network specific operations necessary
  to free a receive packet.

  This routine is called by ::EslSocketPortCloseTxDone to free a
  receive packet.

  @param [in] pPacket         Address of an ::ESL_PACKET structure.
  @param [in, out] pRxBytes   Address of the count of RX bytes

**/
VOID
EslIp4PortClosePacketFree (
  IN ESL_PACKET * pPacket,
  IN OUT size_t * pRxBytes
  )
{
  //
  //  Account for the receive bytes
  //
  *pRxBytes -= pPacket->Op.Ip4Rx.pRxData->DataLength;

  //
  //  Return the buffer to the IP4 driver
  //
  gBS->SignalEvent ( pPacket->Op.Ip4Rx.pRxData->RecycleSignal );
}


/**
  Perform the network specific close operation on the port.

  This routine performs a cancel operations on the IPv4 port to
  shutdown the receive operations on the port.

  This routine is called by the ::EslSocketPortCloseTxDone
  routine after the port completes all of the transmission.

  @param [in] pPort           Address of an ::ESL_PORT structure.

  @retval EFI_SUCCESS         The port is closed, not normally returned
  @retval EFI_NOT_READY       The port is still closing
  @retval EFI_ALREADY_STARTED Error, the port is in the wrong state,
                              most likely the routine was called already.

**/
EFI_STATUS
EslIp4PortCloseRxStop (
  IN ESL_PORT * pPort
  )
{
  ESL_IP4_CONTEXT * pIp4;
  EFI_IP4_PROTOCOL * pIp4Protocol;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Reset the port, cancel the outstanding receive
  //
  pIp4 = &pPort->Context.Ip4;
  pIp4Protocol = pPort->pProtocol.IPv4;
  Status = pIp4Protocol->Configure ( pIp4Protocol,
                                      NULL );
  if ( !EFI_ERROR ( Status )) {
    DEBUG (( pPort->DebugFlags | DEBUG_CLOSE | DEBUG_INFO,
              "0x%08x: Port reset\r\n",
              pPort ));
  }
  else {
    DEBUG (( DEBUG_ERROR | pPort->DebugFlags | DEBUG_CLOSE | DEBUG_INFO,
             "ERROR - Port 0x%08x reset failed, Status: %r\r\n",
             pPort,
             Status ));
  }

  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Receive data from a network connection.

  This routine attempts to return buffered data to the caller.  The
  data is only removed from the normal queue, the message flag
  MSG_OOB is ignored.  See the \ref Ip4ReceiveEngine section.

  This routine is called by ::EslSocketReceive to handle the network
  specific receive operation to support SOCK_RAW sockets.

  @param [in] pSocket         Address of an ::ESL_SOCKET structure

  @param [in] Flags           Message control flags

  @param [in] BufferLength    Length of the the buffer

  @param [in] pBuffer         Address of a buffer to receive the data.

  @param [in] pDataLength     Number of received data bytes in the buffer.

  @param [out] pAddress       Network address to receive the remote system address

  @param [in,out] pAddressLength  Length of the remote network address structure

  @retval EFI_SUCCESS - Socket data successfully received

**/
EFI_STATUS
EslIp4Receive (
  IN ESL_SOCKET * pSocket,
  IN INT32 Flags,
  IN size_t BufferLength,
  IN UINT8 * pBuffer,
  OUT size_t * pDataLength,
  OUT struct sockaddr * pAddress,
  IN OUT socklen_t * pAddressLength
  )
{
  socklen_t AddressLength;
  size_t BytesToCopy;
  size_t DataBytes;
  UINT32 Fragment;
  size_t HeaderBytes;
  in_addr_t IpAddress;
  size_t LengthInBytes;
  UINT8 * pData;
  ESL_PACKET * pPacket;
  ESL_PORT * pPort;
  struct sockaddr_in * pRemoteAddress;
  EFI_IP4_RECEIVE_DATA * pRxData;
  ESL_IP4_CONTEXT * pIp4;
  struct sockaddr_in RemoteAddress;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Assume failure
  //
  Status = EFI_UNSUPPORTED;
  pSocket->errno = ENOTCONN;

  //
  //  Verify that the socket is connected
  //
  if (( SOCKET_STATE_CONNECTED == pSocket->State )
    || ( PORT_STATE_RX_ERROR == pSocket->State )) {
    //
    //  Locate the port
    //
    pPort = pSocket->pPortList;
    if ( NULL != pPort ) {
      //
      //  Determine if there is any data on the queue
      //
      pIp4 = &pPort->Context.Ip4;
      pPacket = pSocket->pRxPacketListHead;
      if ( NULL != pPacket ) {
        //
        //  Validate the return address parameters
        //
        pRxData = pPacket->Op.Ip4Rx.pRxData;
        if (( NULL == pAddress ) || ( NULL != pAddressLength )) {
          //
          //  Return the remote system address if requested
          //
          if ( NULL != pAddress ) {
            //
            //  Build the remote address
            //
            DEBUG (( DEBUG_RX,
                      "Getting packet source address: %d.%d.%d.%d\r\n",
                      pRxData->Header->SourceAddress.Addr[0],
                      pRxData->Header->SourceAddress.Addr[1],
                      pRxData->Header->SourceAddress.Addr[2],
                      pRxData->Header->SourceAddress.Addr[3]));
            ZeroMem ( &RemoteAddress, sizeof ( RemoteAddress ));
            RemoteAddress.sin_len = sizeof ( RemoteAddress );
            RemoteAddress.sin_family = AF_INET;
            IpAddress = pRxData->Header->SourceAddress.Addr[3];
            IpAddress <<= 8;
            IpAddress |= pRxData->Header->SourceAddress.Addr[2];
            IpAddress <<= 8;
            IpAddress |= pRxData->Header->SourceAddress.Addr[1];
            IpAddress <<= 8;
            IpAddress |= pRxData->Header->SourceAddress.Addr[0];
            RemoteAddress.sin_addr.s_addr = IpAddress;

            //
            //  Copy the address
            //
            pRemoteAddress = (struct sockaddr_in *)pAddress;
            AddressLength = sizeof ( *pRemoteAddress );
            if ( AddressLength > *pAddressLength ) {
              AddressLength = *pAddressLength;
            }
            CopyMem ( pRemoteAddress,
                      &RemoteAddress,
                      AddressLength );

            //
            //  Update the address length
            //
            *pAddressLength = AddressLength;
          }

          //
          //  Reduce the buffer length if necessary
          //
          DataBytes = pRxData->HeaderLength + pRxData->DataLength;
          if ( DataBytes < BufferLength ) {
            BufferLength = DataBytes;
          }

          //
          //  Copy the IP header
          //
          LengthInBytes = 0;
          HeaderBytes = pRxData->HeaderLength;
          if ( HeaderBytes > BufferLength ) {
            HeaderBytes = BufferLength;
          }
          DEBUG (( DEBUG_RX,
                    "0x%08x: Port copy header 0x%08x data into 0x%08x, 0x%08x bytes\r\n",
                    pPort,
                    pRxData->Header,
                    pBuffer,
                    HeaderBytes ));
          CopyMem ( pBuffer, pRxData->Header, HeaderBytes );
          pBuffer += HeaderBytes;
          LengthInBytes += HeaderBytes;
          
          //
          //  Copy the received data
          //
          Fragment = 0;
          while ( BufferLength > LengthInBytes ) {
            //
            //  Determine the amount of received data
            //
            pData = pRxData->FragmentTable[Fragment].FragmentBuffer;
            BytesToCopy = pRxData->FragmentTable[Fragment].FragmentLength;
            if (( BufferLength - LengthInBytes ) < BytesToCopy ) {
              BytesToCopy = BufferLength - LengthInBytes;
            }
            LengthInBytes += BytesToCopy;

            //
            //  Move the data into the buffer
            //
            DEBUG (( DEBUG_RX,
                      "0x%08x: Port copy packet 0x%08x data into 0x%08x, 0x%08x bytes\r\n",
                      pPort,
                      pPacket,
                      pBuffer,
                      BytesToCopy ));
            CopyMem ( pBuffer, pData, BytesToCopy );
            pBuffer += BytesToCopy;
          }

          //
          //  Determine if the data is being read
          //
          if ( 0 == ( Flags & MSG_PEEK )) {
            //
            //  Display for the bytes consumed
            //
            DEBUG (( DEBUG_RX,
                      "0x%08x: Port account for 0x%08x bytes\r\n",
                      pPort,
                      BufferLength ));

            //
            //  All done with this packet
            //  Account for any discarded data
            //
            pSocket->RxBytes -= DataBytes;
            if ( 0 != ( DataBytes - BufferLength )) {
              DEBUG (( DEBUG_RX,
                        "0x%08x: Port, packet read, skipping over 0x%08x bytes\r\n",
                        pPort,
                        DataBytes - BufferLength ));
            }

            //
            //  Remove this packet from the queue
            //
            pSocket->pRxPacketListHead = pPacket->pNext;
            if ( NULL == pSocket->pRxPacketListHead ) {
              pSocket->pRxPacketListTail = NULL;
            }

            //
            //  Return this packet to the IP4 driver
            //
            gBS->SignalEvent ( pRxData->RecycleSignal );

            //
            //  Move the packet to the free queue
            //
            pPacket->pNext = pSocket->pRxFree;
            pSocket->pRxFree = pPacket;
            DEBUG (( DEBUG_RX,
                      "0x%08x: Port freeing packet 0x%08x\r\n",
                      pPort,
                      pPacket ));

            //
            //  Restart this receive operation if necessary
            //
            if (( NULL == pPort->pReceivePending )
              && ( MAX_RX_DATA > pSocket->RxBytes )) {
                EslIp4RxStart ( pPort );
            }
          }

          //
          //  Return the data length
          //
          *pDataLength = LengthInBytes;

          //
          //  Successful operation
          //
          Status = EFI_SUCCESS;
          pSocket->errno = 0;
        }
        else {
          //
          //  Bad return address pointer and length
          //
          Status = EFI_INVALID_PARAMETER;
          pSocket->errno = EINVAL;
        }
      }
      else {
        //
        //  The queue is empty
        //  Determine if it is time to return the receive error
        //
        if ( EFI_ERROR ( pSocket->RxError )) {
          Status = pSocket->RxError;
          switch ( Status ) {
          default:
            pSocket->errno = EIO;
            break;

          case EFI_HOST_UNREACHABLE:
            pSocket->errno = EHOSTUNREACH;
            break;

          case EFI_NETWORK_UNREACHABLE:
            pSocket->errno = ENETUNREACH;
            break;

          case EFI_PORT_UNREACHABLE:
            pSocket->errno = EPROTONOSUPPORT;
            break;

          case EFI_PROTOCOL_UNREACHABLE:
            pSocket->errno = ENOPROTOOPT;
            break;
          }
          pSocket->RxError = EFI_SUCCESS;
        }
        else {
          Status = EFI_NOT_READY;
          pSocket->errno = EAGAIN;
        }
      }
    }
  }

  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Cancel the receive operations

  This routine cancels the pending receive operations.
  See the \ref Ip4ReceiveEngine section.

  This routine is called by ::EslSocketShutdown when the socket
  layer is being shutdown.

  @param [in] pSocket   Address of an ::ESL_SOCKET structure
  
  @retval EFI_SUCCESS - The cancel was successful

 **/
EFI_STATUS
EslIp4RxCancel (
  IN ESL_SOCKET * pSocket
  )
{
  ESL_PACKET * pPacket;
  ESL_PORT * pPort;
  ESL_IP4_CONTEXT * pIp4;
  EFI_IP4_PROTOCOL * pIp4Protocol;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Assume failure
  //
  Status = EFI_NOT_FOUND;

  //
  //  Locate the port
  //
  pPort = pSocket->pPortList;
  if ( NULL != pPort ) {
    //
    //  Determine if a receive is pending
    //
    pIp4 = &pPort->Context.Ip4;
    pPacket = pPort->pReceivePending;
    if ( NULL != pPacket ) {
      //
      //  Attempt to cancel the receive operation
      //
      pIp4Protocol = pPort->pProtocol.IPv4;
      Status = pIp4Protocol->Cancel ( pIp4Protocol,
                                       &pIp4->RxToken );
      if ( EFI_NOT_FOUND == Status ) {
        //
        //  The receive is complete
        //
        Status = EFI_SUCCESS;
      }
    }
  }

  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Process the receive completion

  This routine keeps the IPv4 driver's buffer and queues it in
  in FIFO order to the data queue.  The IP4 driver's buffer will
  be returned by either ::EslIp4Receive or ::EslSocketPortCloseTxDone.
  See the \ref Tcp4ReceiveEngine section.

  This routine is called by the IPv4 driver when data is
  received.

  @param [in] Event     The receive completion event

  @param [in] pPort     The address of an ::ESL_PORT structure

**/
VOID
EslIp4RxComplete (
  IN EFI_EVENT Event,
  IN ESL_PORT * pPort
  )
{
  size_t LengthInBytes;
  ESL_PACKET * pPacket;
  ESL_PACKET * pPrevious;
  EFI_IP4_RECEIVE_DATA * pRxData;
  ESL_SOCKET * pSocket;
  ESL_IP4_CONTEXT * pIp4;
  EFI_STATUS Status;
  
  DBG_ENTER ( );
  
  //
  //  Mark this receive complete
  //
  pIp4 = &pPort->Context.Ip4;
  pPacket = pPort->pReceivePending;
  pPort->pReceivePending = NULL;
  
  //
  //  Determine if this receive was successful
  //
  pSocket = pPort->pSocket;
  Status = pIp4->RxToken.Status;
  if (( !EFI_ERROR ( Status )) && ( !pSocket->bRxDisable )) {
    pRxData = pIp4->RxToken.Packet.RxData;
    if ( PORT_STATE_CLOSE_STARTED >= pPort->State ) {
      //
      //  Save the data in the packet
      //
      pPacket->Op.Ip4Rx.pRxData = pRxData;

      //
      //  Queue this packet
      //
      pPrevious = pSocket->pRxPacketListTail;
      if ( NULL == pPrevious ) {
        pSocket->pRxPacketListHead = pPacket;
      }
      else {
        pPrevious->pNext = pPacket;
      }
      pSocket->pRxPacketListTail = pPacket;

      //
      //  Account for the data
      //
      LengthInBytes = pRxData->HeaderLength + pRxData->DataLength;
      pSocket->RxBytes += LengthInBytes;

      //
      //  Log the received data
      //
      DEBUG (( DEBUG_RX | DEBUG_INFO,
                "Received packet from: %d.%d.%d.%d\r\n",
                pRxData->Header->SourceAddress.Addr[0],
                pRxData->Header->SourceAddress.Addr[1],
                pRxData->Header->SourceAddress.Addr[2],
                pRxData->Header->SourceAddress.Addr[3]));
      DEBUG (( DEBUG_RX | DEBUG_INFO,
                "Received packet sent to: %d.%d.%d.%d\r\n",
                pRxData->Header->DestinationAddress.Addr[0],
                pRxData->Header->DestinationAddress.Addr[1],
                pRxData->Header->DestinationAddress.Addr[2],
                pRxData->Header->DestinationAddress.Addr[3]));
      DEBUG (( DEBUG_RX | DEBUG_INFO,
                "0x%08x: Packet queued on port 0x%08x with 0x%08x bytes of data\r\n",
                pPacket,
                pPort,
                LengthInBytes ));

      //
      //  Attempt to restart this receive operation
      //
      if ( pSocket->MaxRxBuf > pSocket->RxBytes ) {
        EslIp4RxStart ( pPort );
      }
      else {
        DEBUG (( DEBUG_RX,
                  "0x%08x: Port RX suspended, 0x%08x bytes queued\r\n",
                  pPort,
                  pSocket->RxBytes ));
      }
    }
    else {
      //
      //  The port is being closed
      //  Return the buffer to the IP4 driver
      //
      gBS->SignalEvent ( pRxData->RecycleSignal );

      //
      //  Free the packet
      //
      EslSocketPacketFree ( pPacket, DEBUG_RX );
    }
  }
  else {
    DEBUG (( DEBUG_RX | DEBUG_INFO,
              "ERROR - Receiving packet 0x%08x, on port 0x%08x, Status:%r\r\n",
              pPacket,
              pPort,
              Status ));
  
    //
    //  Receive error, free the packet save the error
    //
    EslSocketPacketFree ( pPacket, DEBUG_RX );
    if ( !EFI_ERROR ( pSocket->RxError )) {
      pSocket->RxError = Status;
    }
  
    //
    //  Update the port state
    //
    if ( PORT_STATE_CLOSE_STARTED <= pPort->State ) {
      EslSocketPortCloseRxDone ( pPort );
    }
    else {
      if ( EFI_ERROR ( Status )) {
        pPort->State = PORT_STATE_RX_ERROR;
      }
    }
  }
  
  DBG_EXIT ( );
}


/**
  Start a receive operation

  This routine posts a receive buffer to the IPv4 driver.
  See the \ref Ip4ReceiveEngine section.

  This support routine is called by:
  <ul>
    <li>::EslIp4SocketIsConfigured to start the recevie engine for the new socket.</li>
    <li>::EslIp4Receive to restart the receive engine to release flow control.</li>
    <li>::EslIp4RxComplete to continue the operation of the receive engine if flow control is not being applied.</li>
  </ul>

  @param [in] pPort       Address of an ::ESL_PORT structure.

 **/
VOID
EslIp4RxStart (
  IN ESL_PORT * pPort
  )
{
  ESL_PACKET * pPacket;
  ESL_SOCKET * pSocket;
  ESL_IP4_CONTEXT * pIp4;
  EFI_IP4_PROTOCOL * pIp4Protocol;
  EFI_STATUS Status;

  DBG_ENTER ( );

  //
  //  Determine if a receive is already pending
  //
  Status = EFI_SUCCESS;
  pPacket = NULL;
  pSocket = pPort->pSocket;
  pIp4 = &pPort->Context.Ip4;
  if ( !EFI_ERROR ( pPort->pSocket->RxError )) {
    if (( NULL == pPort->pReceivePending )
      && ( PORT_STATE_CLOSE_STARTED > pPort->State )) {
      //
      //  Determine if there are any free packets
      //
      pPacket = pSocket->pRxFree;
      if ( NULL != pPacket ) {
        //
        //  Remove this packet from the free list
        //
        pSocket->pRxFree = pPacket->pNext;
        DEBUG (( DEBUG_RX,
                  "0x%08x: Port removed packet 0x%08x from free list\r\n",
                  pPort,
                  pPacket ));
      }
      else {
        //
        //  Allocate a packet structure
        //
        Status = EslSocketPacketAllocate ( &pPacket,
                                           sizeof ( pPacket->Op.Ip4Rx ),
                                           DEBUG_RX );
        if ( EFI_ERROR ( Status )) {
          pPacket = NULL;
          DEBUG (( DEBUG_ERROR | DEBUG_RX,
                    "0x%08x: Port failed to allocate RX packet, Status: %r\r\n",
                    pPort,
                    Status ));
        }
      }

      //
      //  Determine if a packet is available
      //
      if ( NULL != pPacket ) {
        //
        //  Initialize the buffer for receive
        //
        pPacket->pNext = NULL;
        pPacket->Op.Ip4Rx.pRxData = NULL;
        pIp4->RxToken.Packet.RxData = NULL;
        pPort->pReceivePending = pPacket;

        //
        //  Start the receive on the packet
        //
        pIp4Protocol = pPort->pProtocol.IPv4;
        Status = pIp4Protocol->Receive ( pIp4Protocol,
                                          &pIp4->RxToken );
        if ( !EFI_ERROR ( Status )) {
          DEBUG (( DEBUG_RX | DEBUG_INFO,
                    "0x%08x: Packet receive pending on port 0x%08x\r\n",
                    pPacket,
                    pPort ));
        }
        else {
          DEBUG (( DEBUG_RX | DEBUG_INFO,
                    "ERROR - Failed to post a receive on port 0x%08x, Status: %r\r\n",
                    pPort,
                    Status ));
          if ( !EFI_ERROR ( pSocket->RxError )) {
            //
            //  Save the error status
            //
            pSocket->RxError = Status;
          }

          //
          //  Free the packet
          //
          pPort->pReceivePending = NULL;
          pPacket->pNext = pSocket->pRxFree;
          pSocket->pRxFree = pPacket;
        }
      }
    }
  }

  DBG_EXIT ( );
}


/**
  Shutdown the IP4 service.

  This routine undoes the work performed by ::EslIp4Initialize to
  shutdown the IP4 service which is used by the sockets layer to
  support SOCK_RAW sockets.

  This routine is called by ::EslServiceDisconnect prior to freeing
  the ::ESL_SERVICE structure associated with the adapter running
  IPv4.

  @param [in] pService    The address of an ::ESL_SERVICE structure

**/
VOID
EFIAPI
EslIp4Shutdown (
  IN ESL_SERVICE * pService
  )
{
  ESL_LAYER * pLayer;
  ESL_PORT * pPort;
  ESL_SERVICE * pPreviousService;

  DBG_ENTER ( );

  //
  //  Verify the socket layer synchronization
  //
  VERIFY_TPL ( TPL_SOCKETS );

  //
  //  Walk the list of ports
  //
  do {
    pPort = pService->pPortList;
    if ( NULL != pPort ) {
      //
      //  Remove the port from the port list
      //
      pService->pPortList = pPort->pLinkService;

      //
      //  Close the port
      // TODO: Fix this
      //
//      pPort->pfnClosePort ( pPort, 0 );
    }
  } while ( NULL != pPort );

  //
  //  Remove the service from the service list
  //
  pLayer = &mEslLayer;
  pPreviousService = pLayer->pIp4List;
  if ( pService == pPreviousService ) {
    //
    //  Remove the service from the beginning of the list
    //
    pLayer->pIp4List = pService->pNext;
  }
  else {
    //
    //  Remove the service from the middle of the list
    //
    while ( NULL != pPreviousService ) {
      if ( pService == pPreviousService->pNext ) {
        pPreviousService->pNext = pService->pNext;
        break;
      }
      pPreviousService = pPreviousService->pNext;
    }
  }

  DBG_EXIT ( );
}


/**
  Determine if the socket is configured.

  This routine uses the flag ESL_SOCKET::bConfigured to determine
  if the network layer's configuration routine has been called.
  This routine calls the bind and configuration routines if they
  were not already called.  After the port is configured, the
  \ref Ip4ReceiveEngine is started.

  This routine is called by EslSocketIsConfigured to verify
  that the socket is configured.

  @param [in] pSocket         Address of an ::ESL_SOCKET structure
  
  @retval EFI_SUCCESS - The port is connected
  @retval EFI_NOT_STARTED - The port is not connected

 **/
 EFI_STATUS
 EslIp4SocketIsConfigured (
  IN ESL_SOCKET * pSocket
  )
{
  ESL_PORT * pPort;
  ESL_PORT * pNextPort;
  ESL_IP4_CONTEXT * pIp4;
  EFI_IP4_PROTOCOL * pIp4Protocol;
  EFI_STATUS Status;
  struct sockaddr_in LocalAddress;

  DBG_ENTER ( );

  //
  //  Assume success
  //
  Status = EFI_SUCCESS;

  //
  //  Configure the port if necessary
  //
  if ( !pSocket->bConfigured ) {
    //
    //  Fill in the port list if necessary
    //
    if ( NULL == pSocket->pPortList ) {
      LocalAddress.sin_len = sizeof ( LocalAddress );
      LocalAddress.sin_family = AF_INET;
      LocalAddress.sin_addr.s_addr = 0;
      LocalAddress.sin_port = 0;
      Status = EslIp4Bind ( pSocket,
                             (struct sockaddr *)&LocalAddress,
                             LocalAddress.sin_len );
    }

    //
    //  Walk the port list
    //
    pPort = pSocket->pPortList;
    while ( NULL != pPort ) {
      //
      //  Update the raw setting
      //
      pIp4 = &pPort->Context.Ip4;
      if ( pSocket->bIncludeHeader ) {
        //
        //  IP header will be included with the data on transmit
        //
        pIp4->ModeData.ConfigData.RawData = TRUE;
      }

      //
      //  Attempt to configure the port
      //
      pNextPort = pPort->pLinkSocket;
      pIp4Protocol = pPort->pProtocol.IPv4;
      DEBUG (( DEBUG_TX,
                "0x%08x: pPort Configuring for %d.%d.%d.%d --> %d.%d.%d.%d\r\n",
                          pPort,
                          pIp4->ModeData.ConfigData.StationAddress.Addr[0],
                          pIp4->ModeData.ConfigData.StationAddress.Addr[1],
                          pIp4->ModeData.ConfigData.StationAddress.Addr[2],
                          pIp4->ModeData.ConfigData.StationAddress.Addr[3],
                          pIp4->DestinationAddress.Addr[0],
                          pIp4->DestinationAddress.Addr[1],
                          pIp4->DestinationAddress.Addr[2],
                          pIp4->DestinationAddress.Addr[3]));
      Status = pIp4Protocol->Configure ( pIp4Protocol,
                                          &pIp4->ModeData.ConfigData );
      if ( !EFI_ERROR ( Status )) {
        //
        //  Update the configuration data
        //
        Status = pIp4Protocol->GetModeData ( pIp4Protocol,
                                             &pIp4->ModeData,
                                             NULL,
                                             NULL );
      }
      if ( EFI_ERROR ( Status )) {
        DEBUG (( DEBUG_LISTEN,
                  "ERROR - Failed to configure the Ip4 port, Status: %r\r\n",
                  Status ));
        switch ( Status ) {
        case EFI_ACCESS_DENIED:
          pSocket->errno = EACCES;
          break;

        default:
        case EFI_DEVICE_ERROR:
          pSocket->errno = EIO;
          break;

        case EFI_INVALID_PARAMETER:
          pSocket->errno = EADDRNOTAVAIL;
          break;

        case EFI_NO_MAPPING:
          pSocket->errno = EAFNOSUPPORT;
          break;

        case EFI_OUT_OF_RESOURCES:
          pSocket->errno = ENOBUFS;
          break;

        case EFI_UNSUPPORTED:
          pSocket->errno = EOPNOTSUPP;
          break;
        }
      }
      else {
        DEBUG (( DEBUG_LISTEN,
                  "0x%08x: Port configured\r\n",
                  pPort ));
        pPort->bConfigured = TRUE;

        //
        //  Start the first read on the port
        //
        EslIp4RxStart ( pPort );

        //
        //  The socket is connected
        //
        pSocket->State = SOCKET_STATE_CONNECTED;
      }

      //
      //  Set the next port
      //
      pPort = pNextPort;
    }

    //
    //  Determine the configuration status
    //
    if ( NULL != pSocket->pPortList ) {
      pSocket->bConfigured = TRUE;
    }
  }

  //
  //  Determine the socket configuration status
  //
  if ( !EFI_ERROR ( Status )) {
    Status = pSocket->bConfigured ? EFI_SUCCESS : EFI_NOT_STARTED;
  }
  
  //
  //  Return the port connected state.
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Buffer data for transmission over a network connection.

  This routine buffers data for the transmit engine in the normal
  data queue.  When the \ref TransmitEngine has resources, this
  routine will start the transmission of the next buffer on the
  network connection.

  This routine is called by ::EslSocketTransmit to buffer
  data for transmission.  The data is copied into a local buffer
  freeing the application buffer for reuse upon return.  When
  necessary, this routine starts the transmit engine that
  performs the data transmission on the network connection.  The
  transmit engine transmits the data a packet at a time over the
  network connection.

  Transmission errors are returned during the next transmission or
  during the close operation.  Only buffering errors are returned
  during the current transmission attempt.

  @param [in] pSocket         Address of an ::ESL_SOCKET structure

  @param [in] Flags           Message control flags

  @param [in] BufferLength    Length of the the buffer

  @param [in] pBuffer         Address of a buffer to receive the data.

  @param [in] pDataLength     Number of received data bytes in the buffer.

  @param [in] pAddress        Network address of the remote system address

  @param [in] AddressLength   Length of the remote network address structure

  @retval EFI_SUCCESS - Socket data successfully buffered

**/
EFI_STATUS
EslIp4TxBuffer (
  IN ESL_SOCKET * pSocket,
  IN int Flags,
  IN size_t BufferLength,
  IN CONST UINT8 * pBuffer,
  OUT size_t * pDataLength,
  IN const struct sockaddr * pAddress,
  IN socklen_t AddressLength
  )
{
  ESL_PACKET * pPacket;
  ESL_PACKET * pPreviousPacket;
  ESL_PORT * pPort;
  const struct sockaddr_in * pRemoteAddress;
  ESL_IP4_CONTEXT * pIp4;
  size_t * pTxBytes;
  ESL_IP4_TX_DATA * pTxData;
  EFI_STATUS Status;
  EFI_TPL TplPrevious;

  DBG_ENTER ( );

  //
  //  Assume failure
  //
  Status = EFI_UNSUPPORTED;
  pSocket->errno = ENOTCONN;
  *pDataLength = 0;

  //
  //  Verify that the socket is connected
  //
  if ( SOCKET_STATE_CONNECTED == pSocket->State ) {
    //
    //  Locate the port
    //
    pPort = pSocket->pPortList;
    if ( NULL != pPort ) {
      //
      //  Determine the queue head
      //
      pIp4 = &pPort->Context.Ip4;
      pTxBytes = &pSocket->TxBytes;

      //
      //  Verify that there is enough room to buffer another
      //  transmit operation
      //
      if ( pSocket->MaxTxBuf > *pTxBytes ) {
        //
        //  Attempt to allocate the packet
        //
        Status = EslSocketPacketAllocate ( &pPacket,
                                           sizeof ( pPacket->Op.Ip4Tx )
                                           - sizeof ( pPacket->Op.Ip4Tx.Buffer )
                                           + BufferLength,
                                           DEBUG_TX );
        if ( !EFI_ERROR ( Status )) {
          //
          //  Initialize the transmit operation
          //
          pTxData = &pPacket->Op.Ip4Tx;
          pTxData->TxData.DestinationAddress.Addr[0] = pIp4->DestinationAddress.Addr[0];
          pTxData->TxData.DestinationAddress.Addr[1] = pIp4->DestinationAddress.Addr[1];
          pTxData->TxData.DestinationAddress.Addr[2] = pIp4->DestinationAddress.Addr[2];
          pTxData->TxData.DestinationAddress.Addr[3] = pIp4->DestinationAddress.Addr[3];
          pTxData->TxData.OverrideData = NULL;
          pTxData->TxData.OptionsLength = 0;
          pTxData->TxData.OptionsBuffer = NULL;
          pTxData->TxData.TotalDataLength = (UINT32) BufferLength;
          pTxData->TxData.FragmentCount = 1;
          pTxData->TxData.FragmentTable[0].FragmentLength = (UINT32) BufferLength;
          pTxData->TxData.FragmentTable[0].FragmentBuffer = &pPacket->Op.Ip4Tx.Buffer[0];

          //
          //  Set the remote system address if necessary
          //
          if ( NULL != pAddress ) {
            pRemoteAddress = (const struct sockaddr_in *)pAddress;
            pTxData->Override.SourceAddress.Addr[0] = pIp4->ModeData.ConfigData.StationAddress.Addr[0];
            pTxData->Override.SourceAddress.Addr[1] = pIp4->ModeData.ConfigData.StationAddress.Addr[1];
            pTxData->Override.SourceAddress.Addr[2] = pIp4->ModeData.ConfigData.StationAddress.Addr[2];
            pTxData->Override.SourceAddress.Addr[3] = pIp4->ModeData.ConfigData.StationAddress.Addr[3];
            pTxData->TxData.DestinationAddress.Addr[0] = (UINT8)pRemoteAddress->sin_addr.s_addr;
            pTxData->TxData.DestinationAddress.Addr[1] = (UINT8)( pRemoteAddress->sin_addr.s_addr >> 8 );
            pTxData->TxData.DestinationAddress.Addr[2] = (UINT8)( pRemoteAddress->sin_addr.s_addr >> 16 );
            pTxData->TxData.DestinationAddress.Addr[3] = (UINT8)( pRemoteAddress->sin_addr.s_addr >> 24 );
            pTxData->Override.GatewayAddress.Addr[0] = 0;
            pTxData->Override.GatewayAddress.Addr[1] = 0;
            pTxData->Override.GatewayAddress.Addr[2] = 0;
            pTxData->Override.GatewayAddress.Addr[3] = 0;
            pTxData->Override.Protocol = (UINT8)pSocket->Protocol;
            pTxData->Override.TypeOfService = 0;
            pTxData->Override.TimeToLive = 255;
            pTxData->Override.DoNotFragment = FALSE;

            //
            //  Use the remote system address when sending this packet
            //
            pTxData->TxData.OverrideData = &pTxData->Override;
          }

          //
          //  Copy the data into the buffer
          //
          CopyMem ( &pPacket->Op.Ip4Tx.Buffer[0],
                    pBuffer,
                    BufferLength );

          //
          //  Synchronize with the socket layer
          //
          RAISE_TPL ( TplPrevious, TPL_SOCKETS );

          //
          //  Stop transmission after an error
          //
          if ( !EFI_ERROR ( pSocket->TxError )) {
            //
            //  Display the request
            //
            DEBUG (( DEBUG_TX,
                      "Send %d %s bytes from 0x%08x\r\n",
                      BufferLength,
                      pBuffer ));

            //
            //  Queue the data for transmission
            //
            pPacket->pNext = NULL;
            pPreviousPacket = pSocket->pTxPacketListTail;
            if ( NULL == pPreviousPacket ) {
              pSocket->pTxPacketListHead = pPacket;
            }
            else {
              pPreviousPacket->pNext = pPacket;
            }
            pSocket->pTxPacketListTail = pPacket;
            DEBUG (( DEBUG_TX,
                      "0x%08x: Packet on transmit list\r\n",
                      pPacket ));

            //
            //  Account for the buffered data
            //
            *pTxBytes += BufferLength;
            *pDataLength = BufferLength;

            //
            //  Start the transmit engine if it is idle
            //
            if ( NULL != pPort->pTxFree ) {
              EslSocketTxStart ( pPort,
                                 &pSocket->pTxPacketListHead,
                                 &pSocket->pTxPacketListTail,
                                 &pPort->pTxActive,
                                 &pPort->pTxFree );
            }
          }
          else {
            //
            //  Previous transmit error
            //  Stop transmission
            //
            Status = pSocket->TxError;
            pSocket->errno = EIO;

            //
            //  Free the packet
            //
            EslSocketPacketFree ( pPacket, DEBUG_TX );
          }

          //
          //  Release the socket layer synchronization
          //
          RESTORE_TPL ( TplPrevious );
        }
        else {
          //
          //  Packet allocation failed
          //
          pSocket->errno = ENOMEM;
        }
      }
      else {
        //
        //  Not enough buffer space available
        //
        pSocket->errno = EAGAIN;
        Status = EFI_NOT_READY;
      }
    }
  }

  //
  //  Return the operation status
  //
  DBG_EXIT_STATUS ( Status );
  return Status;
}


/**
  Process the transmit completion

  This routine use ::EslSocketTxComplete to perform the transmit
  completion processing for data packets.

  This routine is called by the IPv4 network layer when a data
  transmit request completes.

  @param [in] Event     The normal transmit completion event

  @param [in] pIo       The address of an ::ESL_IO_MGMT structure

**/
VOID
EslIp4TxComplete (
  IN EFI_EVENT Event,
  IN ESL_IO_MGMT * pIo
  )
{
  UINT32 LengthInBytes;
  ESL_PORT * pPort;
  ESL_PACKET * pPacket;
  ESL_SOCKET * pSocket;
  EFI_STATUS Status;
  
  DBG_ENTER ( );
  
  //
  //  Locate the active transmit packet
  //
  pPacket = pIo->pPacket;
  pPort = pIo->pPort;
  pSocket = pPort->pSocket;

  //
  //  Get the transmit length and status
  //
  LengthInBytes = pPacket->Op.Ip4Tx.TxData.TotalDataLength;
  pSocket->TxBytes -= LengthInBytes;
  Status = pIo->Token.Ip4Tx.Status;

  //
  //  Complete the transmit operation
  //
  EslSocketTxComplete ( pIo,
                        LengthInBytes,
                        Status,
                        "Raw ",
                        &pSocket->pTxPacketListHead,
                        &pSocket->pTxPacketListTail,
                        &pPort->pTxActive,
                        &pPort->pTxFree );
  DBG_EXIT ( );
}