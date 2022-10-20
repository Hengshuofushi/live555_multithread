#ifndef RTSP_CLIENT_WORKER_HPP_
#define RTSP_CLINET_WORKER_HPP_


#include "DynamicRTSPServer.hh"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <functional>
#include <memory>

using ClientTask = std::function<GenericMediaServer::ClientConnection*(DynamicRTSPServer&)>;

class RTSPClientWorker
{
public:
    explicit RTSPClientWorker(size_t id_, UsageEnvironment* usage)
        : id(id_), fakeServer(*usage, 0, 0, 0, nullptr, 65) {}
    ~RTSPClientWorker()
    {
        if (worker.joinable())
        {
            worker.join();
        }
        
    }
    RTSPClientWorker(const RTSPClientWorker&) = delete;
    RTSPClientWorker& operator=(const RTSPClientWorker&) = delete;
    void Start();
    TaskToken AddClientTask(int clientSocket, struct sockaddr_storage const& clientAddr, Boolean useTLS);
    void AddCreateConnectTask();
    UsageEnvironment& envir() const {return fakeServer.envir(); }
    DynamicRTSPServer& server() {return fakeServer; }
    unsigned int Load() const
    {
        std::lock_guard<std::mutex> lck(mtxForClientTask);
        return fakeServer.numClientSessions() + clientTaskQue.size();
    }
private:
    void Work();

    size_t id { 0 };
    mutable std::mutex mtxForClientTask;
    std::vector<ClientTask> clientTaskQue;
    DynamicRTSPServer fakeServer;
    std::thread worker;
};
using RTSPClientWorkerPtr = std::shared_ptr<RTSPClientWorker>;

class RTSPClientWorkerPool
{
public:
    ~RTSPClientWorkerPool() {}
    static RTSPClientWorkerPool& GetInstance()
    {
        static RTSPClientWorkerPool instance;
        return instance;
    }
    void Init(size_t size);
    void AddClientTask(int clientSocket, struct sockaddr_storage const& clientAddr, Boolean useTLS);
private:
    RTSPClientWorkerPool() = default;
    std::vector<RTSPClientWorkerPtr> workerPool;
    
};



#endif