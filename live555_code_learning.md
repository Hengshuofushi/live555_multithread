# live555服务端代码梳理及多线程改造

* 源码整体架构：单线程+事件驱动

## live555服务端代码梳理

### 三大模块

+ UsageEnvironment

  日志输出、事件调度

* liveMedia

  媒体处理相关，媒体会话、媒体文件解析、媒体流传输、RTP/RTCP协议栈
* groupsock

  rtp/rtcp socket管理

### 事件调度机制

* 事件调度器TaskScheduler

  ```c++
  class TaskSchedulerDelay
  {
      void SingleStep();
      ...
      DelayQueue fDelayQueue;
      HandlerSet* fHandlers;
      TaskFunc* fTriggeredEventHandlers[MAX_NUM_EVENT_TRIGGERS];
      ...
  }
  ```

  `fDelayQueue`是定时事件队列，`fHandlers`是socket消息处理器，`fTriggeredEventHandlers`是特定事件触发器(`DeviceSource`单设备处理)。

  `SingleStep()`是线程循环执行体，主要做以下三件事：

1. select监听客户端请求
   select监听所有的socket请求，包括RTSP信令和RTCP RR报文。然后从 `fHandlers`中找到对应的处理器并执行。
2. 执行 `fTriggeredEventHandlers`中已注册的事件处理器。
3. 执行 `fDelayQueue`中到期的事件处理器。

### RTSP信令请求处理流程

两个关键类：`RTSPServer`、`RTSPClientConnection`

`RTSPServer`对象表示一个RTSP服务端，全局唯一。在创建时绑定服务端监听端口，用于监听RTSP请求。封装了 `listen()`、`accept()`接口。

```c++
GenericMediaServer::GenericMediaServer(...)
{
    ...
    env.taskScheduler().turnOnBackgroundReadHandling(fServerSocketIPv4, incomingConnectionHandlerIPv4, this);
    ...
}
```

`turnOnBackgroundReadHandling`将RTSP服务端socket处理器添加到TaskScheduler。当socket事件触发后，TaskScheduler将会调用 `incomingConnectionHandlerIPv4`。

```c++
int GenericMediaServer::setUpOurSocke(...)
{
    if (listen(ourSocket, LISTEN_BACKLOG_SIZE) < 0)
    ...
}
```

```c++
void GenericMediaServer::incomingConnectionHandlerOnSocket(int serverSocket) {
    ...
    int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
    ....
    // Create a new object for handling this connection:
    (void)createNewClientConnection(clientSocket, clientAddr);
}
```

`RTSPClientConnection`类用于处理客户端连接，封装了客户端socket句柄，并对客户端请求实际处理。当 `RTSPServer`调用了  `accept()`接口后创建此对象，每个客户端对应一个此类对象。

`incomingConnectionHandlerIPv4`调用后表明RTSP请求进入，在 `accept()`后，将会调用 `createNewClientConnection`创建 `RTSPClientConnection`对象。

```c++
GenericMediaServer::ClientConnection*
RTSPServer::createNewClientConnection(int clientSocket, struct sockaddr_storage const& clientAddr) {
  return new RTSPClientConnection(*this, clientSocket, clientAddr, fOurConnectionsUseTLS);
}
```

此后客户端的全部请求都将由 `RTSPClientConnection`对象处理。在创建该对象时，注册**客户端socket**对应的处理器：

```c++
GenericMediaServer::ClientConnection
::ClientConnection(...)
{
    ...
    envir().taskScheduler()
    .setBackgroundHandling(fOurSocket, SOCKET_READABLE|SOCKET_EXCEPTION, incomingRequestHandler, this);
    ...
}
```

`incomingRequestHandler`最终调用 `handleRequestBytes`开始处理具体的RTSP消息：

```c++
void RTSPServer::RTSPClientConnection::handleRequestBytes(int newBytesRead) {
    // process OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, etc
    ...
}
```

至此，从RTSP监听到实际处理流程走完。整体流程如下图：

![img](img\live555.png "RTSP信令流程")

### 媒体处理流程

书接上回，在 `RTSPClientConnection::handleRequestBytes`内实际处理RTSP请求。在处理各信令同时对媒体面参数协商、媒体会话管理、媒体传输流程进行处理。下面依次介绍对应各RTSP请求live555的处理。

- DESCRIBE
  此信令主要用于协商客户端和服务端的媒体面信息（IP、端口、编码格式等）。处理函数RTSPClientConnection::handleCmd_DESCRIBE。
  该函数主要做两件事：1.摘要鉴权，2.查找媒体会话。

  ```cpp
  void RTSPServer::RTSPClientConnection
  ::handleCmd_DESCRIB(...)
  {
     ...
     if (!authenticationOK("DESCRIBE", urlTotalSuffix, fullRequestStr)) return;
     fOurServer.lookupServerMediaSession(urlTotalSuffix, DESCRIBELookupCompletionFunction, this);
  }
  ```

  1. 进行摘要鉴权 `RTSPClientConnection::authenticationOK。`

     ```c++
     Boolean RTSPServer::RTSPClientConnection
     ::authenticationOK(...)
     {
         ...
         UserAuthenticationDatabase* authDB = fOurRTSPServer.getAuthenticationDatabaseForCommand(cmdName);
         ...
         char const* password = authDB->lookupPassword(username);
         ...
         char const* ourResponse
           = fCurrentAuthenticator.computeDigestResponse(cmdName, uri);
         success = (strcmp(ourResponse, response) == 0);
         ...
     }
     ```

     注意，`authDB`中的 `username`和 `password`在初始化时设置。

     ```cpp
       authDB = new UserAuthenticationDatabase;
       authDB->addUserRecord("username1", "password1");
     ```

     鉴权不通过则返回401 Unauthorized。否则进入步骤2。
  2. 查找（创建） `ServerMediaSession`会话。

     调用 `lookupServerMediaSession`查找会话，如果找不到这表名是个新的业务，会在这里创建会话并加入缓存。注意在live555中RTSPServer中这个函数并没有创建会话，需要在RTSPServer的继承类中创建。因此会话需要知道媒体文件格式信息，这需要调用方决定。每个 `ServerMediaSession`会话都通过 `streamName`作为键值来索引，而 `streamName`和RTSP请求中携带的URL对应，因此当访问某个URL时，服务端会转换为 内部的 `streamName`先检索 `ServerMediaSession`。

     ```cpp
     void DynamicRTSPServer
     ::lookupServerMediaSession(...)
     {
        ...
        ServerMediaSession* sms = getServerMediaSession(streamName);
        ...
        if (sms == NULL) {
           sms = createNewSMS(envir(), streamName, fid); 
           addServerMediaSession(sms);
        }
        ...
     }
     ```

     找到媒体会话后，会调用传入函数 `DESCRIBELookupCompletionFunction`。在该函数中，会生成SDP报文，并返回200 OK响应。

     ```cpp
     void RTSPServer::RTSPClientConnection::handleCmd_DESCRIBE_afterLooku(...)
     {
        ...
        sdpDescription = session->generateSDPDescription(fAddressFamily);
        if (sdpDescription == NULL) {
           // This usually means that a file name that was specified for a
           // "ServerMediaSubsession" does not exist.
           setRTSPResponse("404 File Not Found, Or In Incorrect Format");
           break;
        }
         ...
     }
     ```

     观察生成SDP报文的函数 `generateSDPDescription`，这里调用了 ` subsession->sdpLines(addressFamily)`来组装SDP的媒体参数相关行(a、m、c、b)，并且还创建了媒体文件源对象 `inputSource`、网络链接对象 `dummyGroupsock`、RTP传输对象 `dummyRTPSink`。

     ```cpp
     char* ServerMediaSession::generateSDPDescription(...)
     {
        ...
        ServerMediaSubsession* subsession;
        for (subsession = fSubsessionsHead; subsession != NULL;
     	 subsession = subsession->fNext) {
           char const* sdpLines = subsession->sdpLines(addressFamily);
           if (sdpLines == NULL) continue; // the media's not available
           sdpLength += strlen(sdpLines);
        }
        ...
     }

     ```

     需要注意，这里的 `subsession`实际上来自于初始化时 ` lookupServerMediaSession`函数，创建并添加 `ServerMediaSession`对象时。初始化时添加支持播放的文件后缀类型，每种文件创建一个 `subSession`并加入 `ServerMediaSession`中。

     ```cpp
     #define NEW_SMS(description) do {\
     char const* descStr = description\
         ", streamed by the LIVE555 Media Server";\
     sms = ServerMediaSession::createNew(env, fileName, fileName, descStr);\
     } while(0)

     static ServerMediaSession* createNewSMS(UsageEnvironment& env,
       charconst* fileName, FILE* /*fid*/)
     {
         ...
         if (strcmp(extension, ".aac") == 0) {
         // Assumed to be an AAC Audio (ADTS format) file:
         NEW_SMS("AAC Audio");
         sms->addSubsession(ADTSAudioFileServerMediaSubsession::createNew(env, fileName, reuseSource));
         }
         ...
     }
     ```

  至此，DESCRIBE流程走完，响应已返回客户端。
- SETUP
  此信令主要用于准备服务端与客户端的媒体面通道。与DESCRIBE信令不同，它是在 `RTSPClientSession::handleCmd_SETUP`正式处理的。
  不过在这之前，会先对SETUP信令鉴权，如果鉴权没过则返回401 Unauthorized，如果鉴权通过则创建 `RTSPClientSession`，再调用 `RTSPClientSession::handleCmd_SETUP`开始处理。这是一个客户端可能同时播放同一个文件，每次播放通过会话区分管理。

  ```cpp
  ...
  if (authenticationOK("SETUP", urlTotalSuffix, (char const*)fRequestBuffer))
  {
      clientSession = (RTSPServer::RTSPClientSession*)fOurRTSPServer.createNewClientSessionWithId();
  }
  else
  {
      areAuthenticated = False;
  }
  ...
  clientSession->handleCmd_SETUP(this, urlPreSuffix, urlSuffix, (char const*)fRequestBuffer);
  playAfterSetup = clientSession->fStreamAfterSETUP;
  ...

  ```

  在 `RTSPClientSession::handleCmd_SETUP`中先调用 `lookupServerMediaSession`查找ServerMediaSession，找到之后调用 `SETUPLookupCompletionFunction1`。之后正式开始处理媒体面参数，先解析RTSP头部的npt、range、IP、端口等参数，然后初始化一下对象做好客户端播放准备。

  ```cpp
  void RTSPServer::RTSPClientSession
  ::handleCmd_SETUP_afterLookup2(...)
  {
     ...
     parseTransportHeader(fFullRequestStr, streamingMode, streamingModeString, clientsDestinationAddressStr, clientsDestinationTTL, clientRTPPortNum, clientRTCPPortNum, rtpChannelId, rtcpChannelId);
     ...
     parseRangeHeader(fFullRequestStr, rangeStart, rangeEnd, absStart, absEnd, startTimeIsNow);
     ...
     subsession->getStreamParameters(fOurSessionId, fOurClientConnection->fClientAddr, clientRTPPort, clientRTCPPort, fStreamStates[trackNum].tcpSocketNum, rtpChannelId, rtcpChannelId, &fOurClientConnection->fTLS, destinationAddress, destinationTTL, fIsMulticast, serverRTPPort, serverRTCPPort, fStreamStates[trackNum].streamToken);
     ...
  }
  ```

  准备工作做完之后，回复200 OK，至此SETUP处理完成。
- PLAY

  PLAY是启动播放的指令。`RTSPClientSession::handleCmd_PLAY`中处理。

  ```cpp
  void RTSPServer::RTSPClientSession
  ::handleCmd_PLAY(...)
  {
      ...
      Boolean sawScaleHeader = parseScaleHeader(fullRequestStr, scale);
      ...
      Boolean sawRangeHeader = parseRangeHeader(fullRequestStr, rangeStart, rangeEnd, absStart, absEnd, startTimeIsNow);
      ...
      fStreamStates[i].subsession->seekStream(fOurSessionId, fStreamStates[i].streamToken, absStart, absEnd);
      ...
      fStreamStates[i].subsession->startStream(fOurSessionId, fStreamStates[i].streamToken, (TaskFunc*)noteClientLiveness, this, rtpSeqNum, rtpTimestamp, RTSPServer::RTSPClientConnection::handleAlternativeRequestByte, ourClientConnection);
      ...
  }
  ```

  先解析scale和range参数，计算播放时间偏移量，然后设置文件流的偏移量，最后开始播放，播放成功后组装响应，返回200 OK。

  进入 `seekStream`，发现需要具体的subSession实现类提供 `seekStreamSource`方法来实现偏移。

  ```cpp
  void OnDemandServerMediaSubsession::seekStream(...)
  {

    StreamState* streamState = (StreamState*)streamToken;
    if (streamState != NULL && streamState->mediaSource() != NULL) {
      seekStreamSource(streamState->mediaSource(), seekNPT, streamDuration, numBytes);
      streamState->startNPT() = (float)seekNPT;
      RTPSink* rtpSink = streamState->rtpSink(); // alias
      if (rtpSink != NULL) rtpSink->resetPresentationTimes();
    }
  }
  ```

  假设要播放mp3文件，则在子类MP3AudioFileServerMediaSubsession处理，`seekWithinFile`表示在文件流中偏移。

  ```cpp
  void MP3AudioFileServerMediaSubsession
  ::seekStreamSource(...)
  {
     ...
    ((MP3FileSource*)sourceMP3Stream)->seekWithinFile(seekNPT, streamDuration);
  }
  ```

  偏移完成后，开始播放 `startStream。`

  ```cpp
  void OnDemandServerMediaSubsession::startStream(...)
  {
      ...
      streamState->startPlaying(destinations, clientSessionId,
  			      rtcpRRHandler, rtcpRRHandlerClientData,
  			      serverRequestAlternativeByteHandler, serverRequestAlternativeByteHandlerClientData);
      ...
  }
  void StreamState::startPlaying(...)
  {
      ...
      fRTCPInstance->sendReport();
      ...
      fRTPSink->startPlaying(*fMediaSource, afterPlayingStreamState, this);
      ...
  }
  ```

  `startPlaying`后续将调用 `buildAndSendPacket`进行组包和发送。

  ```cpp
  void MultiFramedRTPSink::buildAndSendPacket(Boolean isFirstPacket)
  {
      // 组装RTP包
      ...
      packFrame();
  }
  void MultiFramedRTPSink::packFrame()
  {
      ...
      // See if we have an overflow frame that was too big for the last pkt
    if (fOutBuf->haveOverflowData()) {
      ...
      afterGettingFrame1(frameSize, 0, presentationTime, durationInMicroseconds);
    } else {
      // Normal case: we need to read a new frame from the source
      if (fSource == NULL) return;
      fSource->getNextFrame(fOutBuf->curPtr(), fOutBuf->totalBytesAvailable(), afterGettingFrame, this, ourHandleClosure, this);
    }
  }
  ```

  如果发送缓存 `fOutBuf`已经足够一个包，则调用 `afterGettingFrame1`进行发送，否则需要读取文件源的下一帧 `getNextFrame`。

  `getNextFrame`会调用 `doGetNextFrame `实际获取下一帧，这依赖 ` FramedSource`继承类的实现。获取到下一帧后，会调用传入的函数 ` afterGettingFrame` 间接调用 `afterGettingFrame1`。如果读取完毕，则会直接调用 `sendPacketIfNecessary`发包。

  ```cpp
  void MultiFramedRTPSink::ourHandleClosure(void* clientData) {
    MultiFramedRTPSink* sink = (MultiFramedRTPSink*)clientData;
    // There are no frames left, but we may have a partially built packet
    //  to send
    sink->fNoFramesLeft = True;
    sink->sendPacketIfNecessary();
  }
  ```

  进入 `afterGettingFrame1`，开始发送RTP包。

  ```cpp
  void MultiFramedRTPSink::afterGettingFrame1(...)
  {
      ...
      sendPacketIfNecessary();
      ...
  }
  void MultiFramedRTPSink::sendPacketIfNecessary()
  {
      ...
      fRTPInterface.sendPacket(packet, newPacketSize);
      ...
      if (fNoFramesLeft)
      {
        // We're done:
        onSourceClosure();
      }
      else
      {
          ...
          nextTask() = envir().taskScheduler().scheduleDelayedTask(uSecondsToGo, (TaskFunc*)sendNext, this);
      }
  }

  ```

  在 `sendPacketIfNecessary`中，调用 `sendPacket`发送RTP包，然后判断文件源是否已经读完，如果读完了则调用 ` onSourceClosure`清除发送任务，然后调用传入函数就行后续处理 `StreamState::reclaim`，释放缓存对象，释放文件源句柄。

  ```cpp
  void MediaSink::onSourceClosure() {
    // Cancel any pending tasks:
    envir().taskScheduler().unscheduleDelayedTask(nextTask());

    fSource = NULL; // indicates that we can be played again
    if (fAfterFunc != NULL) {
      (*fAfterFunc)(fAfterClientData);
    }
  }
  void StreamState::reclaim() {
    // Delete allocated media objects
    Medium::close(fRTCPInstance) /* will send a RTCP BYE */; fRTCPInstance = NULL;
    Medium::close(fRTPSink); fRTPSink = NULL;
    Medium::close(fUDPSink); fUDPSink = NULL;

    fMaster.closeStreamSource(fMediaSource); fMediaSource = NULL;
    if (fMaster.fLastStreamToken == this) fMaster.fLastStreamToken = NULL;

    delete fRTPgs;
    if (fRTCPgs != fRTPgs) delete fRTCPgs;
    fRTPgs = NULL; fRTCPgs = NULL;
  }
  ```

  否则再创建一个延迟任务  `sendNext`发送下一个包，直到文件读取完成，这样就完成了RTP包的循环发送。

  ```cpp
  void MultiFramedRTPSink::sendNext(void* firstArg)
  {
    MultiFramedRTPSink* sink = (MultiFramedRTPSink*)firstArg;
    sink->buildAndSendPacket(False);
  }
  ```

  PLAY信令处理完成。
- PAUSE

  该信令用于实现暂停播放。再live555源码中的 `RTSPClientSession ::handleCmd_PAUSE`中处理。

  ```cpp
  void RTSPServer::RTSPClientSession
  ::handleCmd_PAUSE(...)
  {
      ...
      fStreamStates[i].subsession->pauseStream(fOurSessionId, fStreamStates[i].streamToken);
      ...
      setRTSPResponse(ourClientConnection, "200 OK", fOurSessionId);
  }
  void StreamState::pause() {
    if (fRTPSink != NULL) fRTPSink->stopPlaying();
    if (fUDPSink != NULL) fUDPSink->stopPlaying();
    if (fMediaSource != NULL) fMediaSource->stopGettingFrames();
    fAreCurrentlyPlaying = False;
  }
  ```

  先停止RTP传输，再停止获取文件下一帧。

  ```cpp
  void MediaSink::stopPlaying() {
    // First, tell the source that we're no longer interested:
    if (fSource != NULL) fSource->stopGettingFrames();

    // Cancel any pending tasks:
    envir().taskScheduler().unscheduleDelayedTask(nextTask());

    fSource = NULL; // indicates that we can be played again
    fAfterFunc = NULL;
  }
  void FramedSource::stopGettingFrames() {
    fIsCurrentlyAwaitingData = False; // indicates that we can be read again
    fAfterGettingFunc = NULL;
    fOnCloseFunc = NULL;

    // Perform any specialized action now:
    doStopGettingFrames();
  }

  void FramedSource::doStopGettingFrames() {
    // Default implementation: Do nothing except cancel any pending 'delivery' task:
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
    // Subclasses may wish to redefine this function.
  }
  ```

  这样媒体流自然停止发送。PAUSE处理完成。
- TEARDOWN

  该信令用于停止播放。再源码的 `RTSPServer::RTSPClientSession::handleCmd_TEARDOWN`函数处理。

  ```cpp
  void RTSPServer::RTSPClientSession
  ::handleCmd_TEARDOWN(RTSPServer::RTSPClientConnection* ourClientConnection,
  		     ServerMediaSubsession* subsession)
  {
      ...
      fStreamStates[i].subsession->deleteStream(fOurSessionId, fStreamStates[i].streamToken);
      ...
      setRTSPResponse(ourClientConnection, "200 OK");
      ...
      if (noSubsessionsRemain) delete this;
      ...
  }
  ```

  先调用 `deleteStream`清除媒体面处理相关对象，然后返回200 OK响应，最后 `delete this`删除本session。
  至此TEARDOWN处理完成。

  最后需要注意，RTSPClientConnection对象何时释放？

  观察 `RTSPClientConnection::handleRequestBytes`，当从socket中读取到异常（`newBytesRead < 0`）则表明TCP已断链，此时调用 `delete this`释放本对象。

  ```cpp
  void RTSPServer::RTSPClientConnection::handleRequestBytes(int newBytesRead)
  {
      ...
      do {
          if (newBytesRead < 0 || (unsigned)newBytesRead >= fRequestBufferBytesLeft)
          {
              fIsActive = False;
              break;
          }
          ...
      } while(0);
      ...
      if (!fIsActive && fScheduledDelayedTask <= 0)
      {
        if (fRecursionCount > 0) closeSockets(); else delete this;
      }
  }
  ```

## 多线程改造

### 改造方案

从RTSP信令处理流程分析，对客户端请求的真正处理集中在 `RTSPClientConnection`中。`RTSPServer`仅仅是在TCP层面接收了客户端请求，并生成客户端socket而已。因此可以很容易将 `RTSPServer`和 `RTSPClientConnection`处理逻辑分离。在处理大量用户请求时，将会处理大量 `RTSPClientConnection`逻辑，服务端压力集中在这部分，因此可将每个客户端对应的 `RTSPClientConnection`交给多线程处理，以此提升处理性能，这样修改对源码侵入也较小，并且符合源码框架。

`accept()`后创建 `RTSPClientConnection`

```c++
GenericMediaServer::ClientConnection*
DynamicRTSPServer::createNewClientConnection(int clientSocket, struct sockaddr_storage const& clientAddr)
{
    RTSPClientWorkerPool::GetInstance().AddClientTask(clientSocket, clientAddr, fOurConnectionsUseTLS);
    return nullptr;
}
```

这里并不真正创建 `RTSPClientConnection`，只是添加一个创建任务到调度器而已。 `AddClientTask`代码如下：

```c++
void RTSPClientWorkerPool::AddClientTask(int clientSocket, struct sockaddr_storage const& clientAddr,
    Boolean useTLS)
{
    auto it = std::min_element(workerPool.begin(), workerPool.end(),
    [this](const RTSPClientWorkerPtr& worker1, const RTSPClientWorkerPtr& worker2)
    {
        return worker1->Load() < worker2->Load();
    });
    if (it == workerPool.end())
    {
        return; 
    }
    (*it)->AddClientTask(clientSocket, clientAddr, useTLS);
}

TaskToken RTSPClientWorker::AddClientTask(int clientSocket, struct sockaddr_storage const& clientAddr, Boolean useTLS)
{
    ClientTask func = std::bind(&createNewClientConnection, std::placeholders::_1, clientSocket, clientAddr, useTLS);
    {
        std::lock_guard<std::mutex> lck(mtxForClientTask);
        clientTaskQue.emplace_back(func);
    }
    return envir().taskScheduler().scheduleDelayedTask(0, CreateClientConnection, this);
}
```

需要注意，创建 `RTSPClientConnection`需要传入 `RTSPServer`对象，但是 `RTSPServer`对象是全局唯一的，并且主线程（RTSP请求监听）和客户端线程（RTSP请求处理）会频繁竞争该对象，访问地方很零散，通过加锁同步的方式会大量侵入源码进行修改，并且竞争效率也不会太高。

分析 `RTSPServer`对 `RTSPClientConnection`的依赖，实际上并不是强依赖，仅仅是作为容器(`fClientConnections`)记录了一下而已，对象间没有深入交互：

```c++
GenericMediaServer::ClientConnection
::ClientConnection(...)
{
    ...
    // Add ourself to our 'client connections' table:
    fOurServer.fClientConnections->Add((char const*)this, this);
    ...
}
```

因此，可在每个客户端线程构造虚拟 `RTSPServer`对象（`fakeServer`），仅作为容器，不参与业务流程，以解决依赖问题。

```c++
class RTSPClientWorker
{
    ...
    DynamicRTSPServer fakeServer;
    ...
};
void CreateClientConnection(void* clientData)
{
    RTSPClientWorker* worker = (RTSPClientWorker*)clientData;
    if (worker != nullptr)
    {
        worker->AddCreateConnectTask();
    }
}

void RTSPClientWorker::AddCreateConnectTask()
{
    std::vector<ClientTask> processList;
    if (!clientTaskQue.empty())
    {
        std::lock_guard<std::mutex> lock(mtxForClientTask);
        processList.swap(clientTaskQue);
    }
    for (ClientTask& task : processList)
    {
        task(fakeServer);
    }
}
```

这样就可以实现多线程处理客户端请求了。整体实现方案流程如下图：

![img](img\live555_multi-thread.png)

代码参考：[live555_multithread](https://github.com/Hengshuofushi/live555_multithread "live555_multithread")

### 测试结果

5个线程6个RTSP请求的情形，测试结果如下：

> // 首列为线程ID：
>
> 139687451682560: DynamicClientConnection:: ctor connection
> 139687443289856: DynamicClientConnection:: ctor connection
> 139687451682560: DynamicClientConnection:: ctor connection
> 139687443289856: DynamicClientConnection:: ctor connection
> 139687451682560: DynamicClientConnection:: ctor connection
> 139687443289856: DynamicClientConnection:: ctor connection

## 后记

当前实现复用了源码的taskScheduler机制，因为源码此机制通过无延迟的死循环轮询处理各类事件（网络IO、延迟任务等），导致CPU占用高达100%。本多线程方案同样有此问题，并且因为多线程原因可能会导致多个CPU占用100%，而其中大部分时间都是空转，相当浪费处理性能。

处理方案：可在 `SingleStep()`增加适当延时，减轻死循环持续占用CPU。如将select改为阻塞+超时方式监听。
