#include "rtsp_client_worker.hh"
#include "BasicUsageEnvironment.hh"
#include <algorithm>

void RTSPClientWorker::Start()
{
    worker = std::thread(&RTSPClientWorker::Work, this);
}

void RTSPClientWorker::Work()
{
    envir().taskScheduler().doEventLoop();
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

GenericMediaServer::ClientConnection*
createNewClientConnection(DynamicRTSPServer& fakeServer, int clientSocket,
    struct sockaddr_storage const& clientAddr, Boolean useTLS)
{
  return new DynamicRTSPServer::DynamicClientConnection(fakeServer, clientSocket, clientAddr, useTLS);
}

void CreateClientConnection(void* clientData)
{
    RTSPClientWorker* worker = (RTSPClientWorker*)clientData;
    if (worker != nullptr)
    {
        worker->AddCreateConnectTask();
    }
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


void RTSPClientWorkerPool::Init(size_t size)
{
    for (size_t i = 1; i <= size; i++)
    {
        TaskScheduler* scheduler = BasicTaskScheduler::createNew();
        UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);
        RTSPClientWorkerPtr worker = std::make_shared<RTSPClientWorker>(i, env);
        worker->Start();
        workerPool.emplace_back(worker);
    }
}

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