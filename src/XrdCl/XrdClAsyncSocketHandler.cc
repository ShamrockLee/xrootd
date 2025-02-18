//------------------------------------------------------------------------------
// Copyright (c) 2011-2012 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#include "XrdCl/XrdClStream.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClMessage.hh"
#include "XrdCl/XrdClAsyncSocketHandler.hh"
#include "XrdCl/XrdClXRootDTransport.hh"
#include "XrdCl/XrdClXRootDMsgHandler.hh"
#include "XrdCl/XrdClOptimizers.hh"
#include "XrdSys/XrdSysE2T.hh"
#include <netinet/tcp.h>

namespace XrdCl
{
  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  AsyncSocketHandler::AsyncSocketHandler( const URL        &url,
                                          Poller           *poller,
                                          TransportHandler *transport,
                                          AnyObject        *channelData,
                                          uint16_t          subStreamNum,
                                          Stream           *strm ):
    pPoller( poller ),
    pTransport( transport ),
    pChannelData( channelData ),
    pSubStreamNum( subStreamNum ),
    pStream( strm ),
    pStreamName( ToStreamName( url, subStreamNum ) ),
    pSocket( new Socket() ),
    pHandShakeDone( false ),
    pConnectionStarted( 0 ),
    pConnectionTimeout( 0 ),
    pHSWaitStarted( 0 ),
    pHSWaitSeconds( 0 ),
    pUrl( url ),
    pTlsHandShakeOngoing( false )
  {
    Env *env = DefaultEnv::GetEnv();

    int timeoutResolution = DefaultTimeoutResolution;
    env->GetInt( "TimeoutResolution", timeoutResolution );
    pTimeoutResolution = timeoutResolution;

    pSocket->SetChannelID( pChannelData );
    pLastActivity = time(0);
  }

  //----------------------------------------------------------------------------
  // Destructor
  //----------------------------------------------------------------------------
  AsyncSocketHandler::~AsyncSocketHandler()
  {
    Close();
    delete pSocket;
  }

  //----------------------------------------------------------------------------
  // Connect to given address
  //----------------------------------------------------------------------------
  XRootDStatus AsyncSocketHandler::Connect( time_t timeout )
  {
    Log *log = DefaultEnv::GetLog();
    pLastActivity = pConnectionStarted = ::time(0);
    pConnectionTimeout = timeout;

    //--------------------------------------------------------------------------
    // Initialize the socket
    //--------------------------------------------------------------------------
    XRootDStatus st = pSocket->Initialize( pSockAddr.Family() );
    if( !st.IsOK() )
    {
      log->Error( AsyncSockMsg, "[%s] Unable to initialize socket: %s",
                  pStreamName.c_str(), st.ToString().c_str() );
      st.status = stFatal;
      return st;
    }

    //--------------------------------------------------------------------------
    // Set the keep-alive up
    //--------------------------------------------------------------------------
    Env *env = DefaultEnv::GetEnv();

    int keepAlive = DefaultTCPKeepAlive;
    env->GetInt( "TCPKeepAlive", keepAlive );
    if( keepAlive )
    {
      int          param = 1;
      XRootDStatus st    = pSocket->SetSockOpt( SOL_SOCKET, SO_KEEPALIVE, &param,
                                          sizeof(param) );
      if( !st.IsOK() )
        log->Error( AsyncSockMsg, "[%s] Unable to turn on keepalive: %s",
                    st.ToString().c_str() );

#if ( defined(__linux__) || defined(__GNU__) ) && defined( TCP_KEEPIDLE ) && \
    defined( TCP_KEEPINTVL ) && defined( TCP_KEEPCNT )

      param = DefaultTCPKeepAliveTime;
      env->GetInt( "TCPKeepAliveTime", param );
      st = pSocket->SetSockOpt(SOL_TCP, TCP_KEEPIDLE, &param, sizeof(param));
      if( !st.IsOK() )
        log->Error( AsyncSockMsg, "[%s] Unable to set keepalive time: %s",
                    st.ToString().c_str() );

      param = DefaultTCPKeepAliveInterval;
      env->GetInt( "TCPKeepAliveInterval", param );
      st = pSocket->SetSockOpt(SOL_TCP, TCP_KEEPINTVL, &param, sizeof(param));
      if( !st.IsOK() )
        log->Error( AsyncSockMsg, "[%s] Unable to set keepalive interval: %s",
                    st.ToString().c_str() );

      param = DefaultTCPKeepAliveProbes;
      env->GetInt( "TCPKeepAliveProbes", param );
      st = pSocket->SetSockOpt(SOL_TCP, TCP_KEEPCNT, &param, sizeof(param));
      if( !st.IsOK() )
        log->Error( AsyncSockMsg, "[%s] Unable to set keepalive probes: %s",
                    st.ToString().c_str() );
#endif
    }

    pHandShakeDone = false;

    //--------------------------------------------------------------------------
    // Initiate async connection to the address
    //--------------------------------------------------------------------------
    char nameBuff[256];
    pSockAddr.Format( nameBuff, sizeof(nameBuff), XrdNetAddrInfo::fmtAdv6 );
    log->Debug( AsyncSockMsg, "[%s] Attempting connection to %s",
                pStreamName.c_str(), nameBuff );

    st = pSocket->ConnectToAddress( pSockAddr, 0 );
    if( !st.IsOK() )
    {
      log->Error( AsyncSockMsg, "[%s] Unable to initiate the connection: %s",
                  pStreamName.c_str(), st.ToString().c_str() );
      return st;
    }

    pSocket->SetStatus( Socket::Connecting );

    //--------------------------------------------------------------------------
    // We should get the ready to write event once we're really connected
    // so we need to listen to it
    //--------------------------------------------------------------------------
    if( !pPoller->AddSocket( pSocket, this ) )
    {
      XRootDStatus st( stFatal, errPollerError );
      pSocket->Close();
      return st;
    }

    if( !pPoller->EnableWriteNotification( pSocket, true, pTimeoutResolution ) )
    {
      XRootDStatus st( stFatal, errPollerError );
      pPoller->RemoveSocket( pSocket );
      pSocket->Close();
      return st;
    }

    return XRootDStatus();
  }

  //----------------------------------------------------------------------------
  // Close the connection
  //----------------------------------------------------------------------------
  XRootDStatus AsyncSocketHandler::Close()
  {
    Log *log = DefaultEnv::GetLog();
    log->Debug( AsyncSockMsg, "[%s] Closing the socket", pStreamName.c_str() );

    pTransport->Disconnect( *pChannelData,
                            pSubStreamNum );

    pPoller->RemoveSocket( pSocket );
    pSocket->Close();
    return XRootDStatus();
  }

  std::string AsyncSocketHandler::ToStreamName( const URL &url, uint16_t strmnb )
  {
    std::ostringstream o;
    o << url.GetHostId();
    o << "." << strmnb;
    return o.str();
  }

  //----------------------------------------------------------------------------
  // Handler a socket event
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::Event( uint8_t type, XrdCl::Socket */*socket*/ )
  {
//    //--------------------------------------------------------------------------
//    // First check if the socket itself wants to apply some mapping on the
//    // event. E.g. in case of TLS socket it might want to map read events to
//    // write events and vice-versa.
//    //--------------------------------------------------------------------------
    type = pSocket->MapEvent( type );

    //--------------------------------------------------------------------------
    // Read event
    //--------------------------------------------------------------------------
    if( type & ReadyToRead )
    {
      pLastActivity = time(0);
      if( unlikely( pTlsHandShakeOngoing ) )
        OnTLSHandShake();
      else if( likely( pHandShakeDone ) )
        OnRead();
      else
        OnReadWhileHandshaking();
    }

    //--------------------------------------------------------------------------
    // Read timeout
    //--------------------------------------------------------------------------
    else if( type & ReadTimeOut )
    {
      if( pHSWaitSeconds )
        CheckHSWait();

      if( likely( pHandShakeDone ) )
        OnReadTimeout();
      else
        OnTimeoutWhileHandshaking();
    }

    //--------------------------------------------------------------------------
    // Write event
    //--------------------------------------------------------------------------
    if( type & ReadyToWrite )
    {
      pLastActivity = time(0);
      if( unlikely( pSocket->GetStatus() == Socket::Connecting ) )
        OnConnectionReturn();
      //------------------------------------------------------------------------
      // Make sure we are not writing anything if we have been told to wait.
      //------------------------------------------------------------------------
      else if( pHSWaitSeconds == 0 )
      {
        if( unlikely( pTlsHandShakeOngoing ) )
          OnTLSHandShake();
        else if( likely( pHandShakeDone ) )
          OnWrite();
        else
          OnWriteWhileHandshaking();
      }
    }

    //--------------------------------------------------------------------------
    // Write timeout
    //--------------------------------------------------------------------------
    else if( type & WriteTimeOut )
    {
      if( likely( pHandShakeDone ) )
        OnWriteTimeout();
      else
        OnTimeoutWhileHandshaking();
    }
  }

  //----------------------------------------------------------------------------
  // Connect returned
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnConnectionReturn()
  {
    //--------------------------------------------------------------------------
    // Check whether we were able to connect
    //--------------------------------------------------------------------------
    Log *log = DefaultEnv::GetLog();
    log->Debug( AsyncSockMsg, "[%s] Async connection call returned",
                pStreamName.c_str() );

    int errorCode = 0;
    socklen_t optSize = sizeof( errorCode );
    XRootDStatus st = pSocket->GetSockOpt( SOL_SOCKET, SO_ERROR, &errorCode,
                                     &optSize );

    //--------------------------------------------------------------------------
    // This is an internal error really (either logic or system fault),
    // so we call it a day and don't retry
    //--------------------------------------------------------------------------
    if( !st.IsOK() )
    {
      log->Error( AsyncSockMsg, "[%s] Unable to get the status of the "
                  "connect operation: %s", pStreamName.c_str(),
                  XrdSysE2T( errno ) );
      pStream->OnConnectError( pSubStreamNum,
                               XRootDStatus( stFatal, errSocketOptError, errno ) );
      return;
    }

    //--------------------------------------------------------------------------
    // We were unable to connect
    //--------------------------------------------------------------------------
    if( errorCode )
    {
      log->Error( AsyncSockMsg, "[%s] Unable to connect: %s",
                  pStreamName.c_str(), XrdSysE2T( errorCode ) );
      pStream->OnConnectError( pSubStreamNum,
                               XRootDStatus( stError, errConnectionError ) );
      return;
    }
    pSocket->SetStatus( Socket::Connected );

    //--------------------------------------------------------------------------
    // Cork the socket
    //--------------------------------------------------------------------------
    st = pSocket->Cork();
    if( !st.IsOK() )
    {
      pStream->OnConnectError( pSubStreamNum, st );
      return;
    }

    //--------------------------------------------------------------------------
    // Initialize the handshake
    //--------------------------------------------------------------------------
    pHandShakeData.reset( new HandShakeData( pStream->GetURL(),
                                        pSubStreamNum ) );
    pHandShakeData->serverAddr = pSocket->GetServerAddress();
    pHandShakeData->clientName = pSocket->GetSockName();
    pHandShakeData->streamName = pStreamName;

    st = pTransport->HandShake( pHandShakeData.get(), *pChannelData );
    if( !st.IsOK() )
    {
      log->Error( AsyncSockMsg, "[%s] Connection negotiation failed",
                  pStreamName.c_str() );
      pStream->OnConnectError( pSubStreamNum, st );
      return;
    }

    if( st.code != suRetry )
      ++pHandShakeData->step;

    //--------------------------------------------------------------------------
    // Initialize the hand-shake reader and writer
    //--------------------------------------------------------------------------
    hswriter.reset( new AsyncHSWriter( *pSocket, pStreamName ) );
    hsreader.reset( new AsyncHSReader( *pTransport, *pSocket, pStreamName, *pStream, pSubStreamNum ) );

    //--------------------------------------------------------------------------
    // Transport has given us something to send
    //--------------------------------------------------------------------------
    if( pHandShakeData->out )
    {
      hswriter->Reset( pHandShakeData->out );
      pHandShakeData->out = nullptr;
    }

    //--------------------------------------------------------------------------
    // Listen to what the server has to say
    //--------------------------------------------------------------------------
    if( !pPoller->EnableReadNotification( pSocket, true, pTimeoutResolution ) )
    {
      hswriter.reset();
      hsreader.reset();
      pStream->OnConnectError( pSubStreamNum,
                               XRootDStatus( stFatal, errPollerError ) );
      return;
    }
  }

  //----------------------------------------------------------------------------
  // Got a write readiness event
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnWrite()
  {
    if( !reqwriter )
    {
      OnFault( XRootDStatus( stError, errInternal, 0, "Request writer is null." ) );
      return;
    }
    //--------------------------------------------------------------------------
    // Let's do the writing ...
    //--------------------------------------------------------------------------
    XRootDStatus st = reqwriter->Write();
    if( !st.IsOK() )
    {
      //------------------------------------------------------------------------
      // We failed
      //------------------------------------------------------------------------
      OnFault( st );
      return;
    }
    //--------------------------------------------------------------------------
    // We are not done yet
    //--------------------------------------------------------------------------
    if( st.code == suRetry) return;
    //--------------------------------------------------------------------------
    // Disable the respective substream if empty
    //--------------------------------------------------------------------------
    reqwriter->Reset();
    pStream->DisableIfEmpty( pSubStreamNum );
  }

  //----------------------------------------------------------------------------
  // Got a write readiness event while handshaking
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnWriteWhileHandshaking()
  {
    XRootDStatus st;
    if( !hswriter || !hswriter->HasMsg() )
    {
      if( !(st = DisableUplink()).IsOK() )
        OnFaultWhileHandshaking( st );
      return;
    }
    //--------------------------------------------------------------------------
    // Let's do the writing ...
    //--------------------------------------------------------------------------
    st = hswriter->Write();
    if( !st.IsOK() )
    {
      //------------------------------------------------------------------------
      // We failed
      //------------------------------------------------------------------------
      OnFaultWhileHandshaking( st );
      return;
    }
    //--------------------------------------------------------------------------
    // We are not done yet
    //--------------------------------------------------------------------------
    if( st.code == suRetry ) return;
    //--------------------------------------------------------------------------
    // Disable the uplink
    // Note: at this point we don't deallocate the HS message as we might need
    //       to re-send it in case of a kXR_wait response
    //--------------------------------------------------------------------------
    if( !(st = DisableUplink()).IsOK() )
      OnFaultWhileHandshaking( st );
  }

  //----------------------------------------------------------------------------
  // Got a read readiness event
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnRead()
  {
    //--------------------------------------------------------------------------
    // Make sure the response reader object exists
    //--------------------------------------------------------------------------
    if( !rspreader )
    {
      OnFault( XRootDStatus( stError, errInternal, 0, "Response reader is null." ) );
      return;
    }

    //--------------------------------------------------------------------------
    // Readout the data from the socket
    //--------------------------------------------------------------------------
    XRootDStatus st = rspreader->Read();

    //--------------------------------------------------------------------------
    // Handler header corruption
    //--------------------------------------------------------------------------
    if( !st.IsOK() && st.code == errCorruptedHeader )
    {
      OnHeaderCorruption();
      return;
    }

    //--------------------------------------------------------------------------
    // Handler other errors
    //--------------------------------------------------------------------------
    if( !st.IsOK() )
    {
      OnFault( st );
      return;
    }

    //--------------------------------------------------------------------------
    // We are not done yet
    //--------------------------------------------------------------------------
    if( st.code == suRetry ) return;

    //--------------------------------------------------------------------------
    // We are done, reset the response reader so we can read out next message
    //--------------------------------------------------------------------------
    rspreader->Reset();
  }

  //----------------------------------------------------------------------------
  // Got a read readiness event while handshaking
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnReadWhileHandshaking()
  {
    //--------------------------------------------------------------------------
    // Make sure the response reader object exists
    //--------------------------------------------------------------------------
    if( !hsreader )
    {
      OnFault( XRootDStatus( stError, errInternal, 0, "Hand-shake reader is null." ) );
      return;
    }

    //--------------------------------------------------------------------------
    // Read the message and let the transport handler look at it when
    // reading has finished
    //--------------------------------------------------------------------------
    XRootDStatus st = hsreader->Read();
    if( !st.IsOK() )
    {
      OnFaultWhileHandshaking( st );
      return;
    }

    if( st.code != suDone )
      return;

    HandleHandShake( hsreader->ReleaseMsg() );
  }

  //------------------------------------------------------------------------
  // Handle the handshake message
  //------------------------------------------------------------------------
  void AsyncSocketHandler::HandleHandShake( std::unique_ptr<Message> msg )
  {
    //--------------------------------------------------------------------------
    // OK, we have a new message, let's deal with it;
    //--------------------------------------------------------------------------
    pHandShakeData->in = msg.release();
    XRootDStatus st = pTransport->HandShake( pHandShakeData.get(), *pChannelData );

    //--------------------------------------------------------------------------
    // Deal with wait responses
    //--------------------------------------------------------------------------
    kXR_int32 waitSeconds = HandleWaitRsp( pHandShakeData->in );

    delete pHandShakeData->in;
    pHandShakeData->in = 0;

    if( !st.IsOK() )
    {
      OnFaultWhileHandshaking( st );
      return;
    }

    if( st.code == suRetry )
    {
      //------------------------------------------------------------------------
      // We are handling a wait response and the transport handler told
      // as to retry the request
      //------------------------------------------------------------------------
      if( waitSeconds >=0 )
      {
        time_t resendTime = ::time( 0 ) + waitSeconds;
        if( resendTime > pConnectionStarted + pConnectionTimeout )
        {
          Log *log = DefaultEnv::GetLog();
          log->Error( AsyncSockMsg,
                      "[%s] Won't retry kXR_endsess request because would"
                      "reach connection timeout.",
                      pStreamName.c_str() );

          OnFaultWhileHandshaking( XRootDStatus( stError, errSocketTimeout ) );
        }
        else
        {
          //--------------------------------------------------------------------
          // We need to wait before replaying the request
          //--------------------------------------------------------------------
          Log *log = DefaultEnv::GetLog();
          log->Debug( AsyncSockMsg, "[%s] Received a wait response to endsess request, "
                      "will wait for %d seconds before replaying the endsess request",
                      waitSeconds );
          pHSWaitStarted = time( 0 );
          pHSWaitSeconds = waitSeconds;
        }
        return;
      }
      //------------------------------------------------------------------------
      // We are re-sending a protocol request
      //------------------------------------------------------------------------
      else if( pHandShakeData->out )
      {
        SendHSMsg();
        return;
      }
    }

    //--------------------------------------------------------------------------
    // If now is the time to enable encryption
    //--------------------------------------------------------------------------
    if( !pSocket->IsEncrypted() &&
         pTransport->NeedEncryption( pHandShakeData.get(), *pChannelData ) )
    {
      XRootDStatus st = DoTlsHandShake();
      if( !st.IsOK() || st.code == suRetry ) return;
    }

    //--------------------------------------------------------------------------
    // Now prepare the next step of the hand-shake procedure
    //--------------------------------------------------------------------------
    HandShakeNextStep( st.IsOK() && st.code == suDone );
  }

  //------------------------------------------------------------------------
  // Prepare the next step of the hand-shake procedure
  //------------------------------------------------------------------------
  void AsyncSocketHandler::HandShakeNextStep( bool done )
  {
    //--------------------------------------------------------------------------
    // We successfully proceeded to the next step
    //--------------------------------------------------------------------------
    ++pHandShakeData->step;

    //--------------------------------------------------------------------------
    // The hand shake process is done
    //--------------------------------------------------------------------------
    if( done )
    {
      pHandShakeData.reset();
      hswriter.reset();
      hsreader.reset();
      //------------------------------------------------------------------------
      // Initialize the request writer & reader
      //------------------------------------------------------------------------
      reqwriter.reset( new AsyncMsgWriter( *pTransport, *pSocket, pStreamName, *pStream, pSubStreamNum, *pChannelData ) );
      rspreader.reset( new AsyncMsgReader( *pTransport, *pSocket, pStreamName, *pStream, pSubStreamNum ) );
      XRootDStatus st;
      if( !(st = EnableUplink()).IsOK() )
      {
        OnFaultWhileHandshaking( st );
        return;
      }
      pHandShakeDone = true;
      pStream->OnConnect( pSubStreamNum );
    }
    //--------------------------------------------------------------------------
    // The transport handler gave us something to write
    //--------------------------------------------------------------------------
    else if( pHandShakeData->out )
    {
      SendHSMsg();
    }
  }

  //----------------------------------------------------------------------------
  // Handle fault
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnFault( XRootDStatus st )
  {
    Log *log = DefaultEnv::GetLog();
    log->Error( AsyncSockMsg, "[%s] Socket error encountered: %s",
                pStreamName.c_str(), st.ToString().c_str() );

    rspreader.reset();
    reqwriter.reset();

    pStream->OnError( pSubStreamNum, st );
  }

  //----------------------------------------------------------------------------
  // Handle fault while handshaking
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnFaultWhileHandshaking( XRootDStatus st )
  {
    Log *log = DefaultEnv::GetLog();
    log->Error( AsyncSockMsg, "[%s] Socket error while handshaking: %s",
                pStreamName.c_str(), st.ToString().c_str() );
    hsreader.reset();
    hswriter.reset();

    pStream->OnConnectError( pSubStreamNum, st );
  }

  //----------------------------------------------------------------------------
  // Handle write timeout
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnWriteTimeout()
  {
    pStream->OnWriteTimeout( pSubStreamNum );
  }

  //----------------------------------------------------------------------------
  // Handler read timeout
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnReadTimeout()
  {
    bool isBroken = false;
    pStream->OnReadTimeout( pSubStreamNum, isBroken );

    if( isBroken )
    {
      rspreader.reset();
      reqwriter.reset();
    }
  }

  //----------------------------------------------------------------------------
  // Handle timeout while handshaking
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnTimeoutWhileHandshaking()
  {
    time_t now = time(0);
    if( now > pConnectionStarted+pConnectionTimeout )
      OnFaultWhileHandshaking( XRootDStatus( stError, errSocketTimeout ) );
  }

  //----------------------------------------------------------------------------
  // Handle header corruption in case of kXR_status response
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::OnHeaderCorruption()
  {
    //--------------------------------------------------------------------------
    // We need to force a socket error so this is handled in a similar way as
    // a stream t/o and all requests are retried
    //--------------------------------------------------------------------------
    pStream->ForceError( XRootDStatus( stError, errSocketError ) );

    rspreader.reset();
    reqwriter.reset();
  }

  //----------------------------------------------------------------------------
  // Carry out the TLS hand-shake
  //----------------------------------------------------------------------------
  XRootDStatus AsyncSocketHandler::DoTlsHandShake()
  {
    Log *log = DefaultEnv::GetLog();
    log->Debug( AsyncSockMsg, "[%s] TLS hand-shake exchange.", pStreamName.c_str() );

    XRootDStatus st;
    if( !( st = pSocket->TlsHandShake( this, pUrl.GetHostName() ) ).IsOK() )
    {
      OnFaultWhileHandshaking( st );
      pTlsHandShakeOngoing = false;
      return st;
    }

    if( st.code == suRetry )
    {
      pTlsHandShakeOngoing = true;
      return st;
    }

    pTlsHandShakeOngoing = false;
    log->Info( AsyncSockMsg, "[%s] TLS hand-shake done.", pStreamName.c_str() );

    return st;
  }

  //----------------------------------------------------------------------------
  // Handle read/write event if we are in the middle of a TLS hand-shake
  //----------------------------------------------------------------------------
  inline void AsyncSocketHandler::OnTLSHandShake()
  {
    XRootDStatus st = DoTlsHandShake();
    if( !st.IsOK() || st.code == suRetry ) return;

    HandShakeNextStep( pTransport->HandShakeDone( pHandShakeData.get(),
                                                  *pChannelData ) );
  }

  //----------------------------------------------------------------------------
  // Prepare a HS writer for sending and enable uplink
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::SendHSMsg()
  {
    if( !hswriter )
    {
      OnFaultWhileHandshaking( XRootDStatus( stError, errInternal, 0,
                                             "HS writer object missing!" ) );
      return;
    }
    //--------------------------------------------------------------------------
    // We only set a new HS message if this is not a replay due to kXR_wait
    //--------------------------------------------------------------------------
    if( !pHSWaitSeconds )
    {
      hswriter->Reset( pHandShakeData->out );
      pHandShakeData->out = nullptr;
    }
    //--------------------------------------------------------------------------
    // otherwise we replay the kXR_endsess request
    //--------------------------------------------------------------------------
    else
      hswriter->Replay();
    //--------------------------------------------------------------------------
    // Enable writing so we can replay the HS message
    //--------------------------------------------------------------------------
    XRootDStatus st;
    if( !(st = EnableUplink()).IsOK() )
    {
      OnFaultWhileHandshaking( st );
      return;
    }
  }

  kXR_int32 AsyncSocketHandler::HandleWaitRsp( Message *msg )
  {
    // It would be more coherent if this could be done in the
    // transport layer, unfortunately the API does not allow it.
    kXR_int32 waitSeconds = -1;
    ServerResponse *rsp = (ServerResponse*)msg->GetBuffer();
    if( rsp->hdr.status == kXR_wait )
      waitSeconds = rsp->body.wait.seconds;
    return waitSeconds;
  }

  //----------------------------------------------------------------------------
  // Check if HS wait time elapsed
  //----------------------------------------------------------------------------
  void AsyncSocketHandler::CheckHSWait()
  {
    time_t now = time( 0 );
    if( now - pHSWaitStarted >= pHSWaitSeconds )
    {
      Log *log = DefaultEnv::GetLog();
      log->Debug( AsyncSockMsg, "[%s] The hand-shake wait time elapsed, will "
                  "replay the endsess request.", pStreamName.c_str() );
      SendHSMsg();
      //------------------------------------------------------------------------
      // Make sure the wait state is reset
      //------------------------------------------------------------------------
      pHSWaitSeconds = 0;
      pHSWaitStarted = 0;
    }
  }
}
