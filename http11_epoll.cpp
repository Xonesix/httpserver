#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <unistd.h>
#include <csignal>
#include <sstream>
#include <memory>
#include <sys/epoll.h>
#include <fcntl.h>
#define PORT 8080 // Defined placeholder for completeness
#define PROTOCOL "HTTP/1.1"
#define MAX_EVENTS 64
#define WORKER_THREADS 3
struct HTTPRequest {
     std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct Worker
{
    std::thread thrd;
    std::atomic<bool> finished{false};
};

class HTTPResponse
{
  /*
   * Example response
   * HTTP/1.1 200 OK\r\n
   * Content-Type: text/plain\r\n
   * Content-Length: 13\r\n
   * Connection: Close\r\n
   *
   * Hello World!
   * \r\n\r\n
   */
  std::ostringstream builder;
  bool doesContentExist = false;
  public:

      HTTPResponse(std::string method, std::string code, std::string humanReadableCode)
      {
          builder << PROTOCOL << " " << code << " " << humanReadableCode << "\r\n";
      }

      void add_content_plain_text(std::string content)
      {
          builder << "Content-Type:" << " " << "text/plain" << "\r\n";
          builder << "Content-Length:" << " " << content.size() << "\r\n";
          builder << "Connection: close\r\n\r\n";
          builder << content;
          doesContentExist = true;
      }

      std::string get_response()
      {


          // if content append
          // if (content) { builder << content;}

          if (!doesContentExist)
          {
            builder << "Content-Length:" << " " << 0 << "\r\n";
            builder << "Connection: close\r\n\r\n";
          }

          return builder.str();
      }


};


struct Connection {
    int fd;
    std::string in;
    std::string out;
    size_t outPos = 0;
    bool watchingWrite = false;
};

struct ClientSession {
    int fd;
    std::string incomplete_buffer; // Persistent storage across events
};


class HTTPServer
{
private:
    std::vector<std::thread> fdToThreadMap = {};
    int serverSocket;
    sockaddr_in serverAddress;
    std::atomic<bool> running;
    std::chrono::milliseconds defaultSleepTime = std::chrono::milliseconds(250);
    std::thread listenerThread;
    std::vector<std::unique_ptr<Worker>> clientThreads{};
    std::vector<std::thread> workerThreads;
    // Epoll vars
    int epoll_fd;        // client sockets, shared by workers (data.ptr = ClientSession*)
    int listen_epoll_fd; // listening socket only, used by the listener thread

public:
    HTTPServer()
    {
        // 1. Create socket FIRST
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);

        if (serverSocket < 0) {
            perror("socket");
            throw std::runtime_error("socket failed");
        }

        // 2. Enable SO_REUSEADDR
        int opt = 1;

        if (setsockopt(serverSocket,
                       SOL_SOCKET,
                       SO_REUSEADDR,
                       &opt,
                       sizeof(opt)) < 0)
        {
            perror("setsockopt");
            close(serverSocket);
            throw std::runtime_error("setsockopt failed");
        }

        // 3. Configure address
        serverAddress = {};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(PORT);
        serverAddress.sin_addr.s_addr = INADDR_ANY;

        // 4. Bind
        if (bind(serverSocket,
                 reinterpret_cast<sockaddr*>(&serverAddress),
                 sizeof(serverAddress)) < 0)
        {
            perror("bind");
            close(serverSocket);
            throw std::runtime_error("bind failed");
        }

        // 5. Listen
        if (listen(serverSocket, 128) < 0)
        {
            perror("listen");
            close(serverSocket);
            throw std::runtime_error("listen failed");
        }


        running.store(true);

        // listenerThread = std::thread(
        //     &HTTPServer::spawnListeningThread, this
        // );


        // Using epoll now
        epoll_fd = epoll_create1(0);
        if(epoll_fd == -1) {
            perror("epoll create failed");
            exit(EXIT_FAILURE);
        }

        // Separate instance for the listener so workers never receive the
        // serverSocket event (its data.fd would be misread as a ClientSession*)
        listen_epoll_fd = epoll_create1(0);
        if(listen_epoll_fd == -1) {
            perror("listen epoll create failed");
            exit(EXIT_FAILURE);
        }

        listenerThread = std::thread(&HTTPServer::spawnListeningThread, this);

        // 4. Spawn fixed pool of Worker Threads exactly once
        for (unsigned int i = 0; i < WORKER_THREADS; ++i) {
            workerThreads.push_back(std::thread(&HTTPServer::spawnWorkerThread, this));
        }

    }
    ~HTTPServer()
    {
        running.store(false);

        // Join listener thread
        if (listenerThread.joinable()) {
            listenerThread.join();
        }

        // Join all worker threads
        for (auto& worker : workerThreads) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        close(listen_epoll_fd);
        close(epoll_fd);
        close(serverSocket);
    }

private:
    void spawnListeningThread()
    {
        // pollfd pfd{};
        // pfd.fd = serverSocket;
        // pfd.events = POLLIN;

        struct epoll_event event;
        // Monitor for incoming data
        event.events = EPOLLIN;
        // Store fd to identify when it fires 
        event.data.fd = serverSocket;
        if (epoll_ctl(listen_epoll_fd, EPOLL_CTL_ADD, serverSocket, &event) == -1) {
            perror("Failed to add serverSocket to epoll");
            return;
        }


        struct epoll_event listen_events[1];

        // Once we get one, we accept and store into a thread map

        // This time we'll add new Fd's to epoll instance
        while (running.load())
        {

            // 1. SLEEP HERE until a new connection arrives (1000ms timeout so we can check running.load())
            int nfds = epoll_wait(listen_epoll_fd, listen_events, 1, 1000);

            if (nfds < 0) {
                if (errno == EINTR) continue; // Interrupted by system signal (like Ctrl+C), try again
                perror("epoll_wait failed in listening thread");
                break;
            }
            
            if (nfds == 0) {
                continue; // Timeout hit with no new connections, loop around to check running.load()
            }

            int tempClientFd = accept(serverSocket, nullptr, nullptr);
            if (tempClientFd < 0) {
                perror("accept failed");
                continue;
            }

            // Set to non blocking  to prevent workers to use a half initialized socket
            
            // fcntl means retrieve flags of a file describter | taking snapshot of settings before making changes
            // F_GETFL means you GETFL...ags of the the file descriptor | 0 is placeholder arg
            // we need this to make this a non-blocking socket
            int flags = fcntl(tempClientFd, F_GETFL, 0);

            if (flags == -1)
            {
                perror("fnctl F_GETFL failed");
                close(tempClientFd);
                continue;
            }

            if(fcntl(tempClientFd, F_SETFL, flags | O_NONBLOCK) == -1)
            {
                perror("FAILED");
                close(tempClientFd);
                continue;
            }

            // Get metadata for client
            ClientSession* session = new ClientSession();
            session->fd = tempClientFd;


            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
            ev.data.ptr = session;

            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tempClientFd, &ev) == -1) {
                perror("epoll_ctl ADD failed");
                delete session;
                close(tempClientFd);
                return;
            }

        }
    }


void spawnWorkerThread()
{
    struct epoll_event events[MAX_EVENTS];
    
    while(running.load())
    {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000); 
        if (nfds < 0) {
            if (errno == EINTR) continue; 
            perror("epoll_wait failed in worker thread");
            break;
        }
        
        for (int i = 0; i < nfds; ++i)
        {
            ClientSession* session = static_cast<ClientSession*>(events[i].data.ptr);
            int client_fd = session->fd;

            bool request_complete = false;
            bool connection_closed = false;
            char buffer[4096];

            while (true)
            {
                ssize_t bytesRead = recv(client_fd, buffer, sizeof(buffer), 0);
                if (bytesRead > 0)
                {
                    // FIX 1: Append data to the heap-allocated session buffer, NOT a local string!
                    session->incomplete_buffer.append(buffer, bytesRead);
                    
                    if (session->incomplete_buffer.find("\r\n\r\n") != std::string::npos)
                    {
                        request_complete = true;
                        break; 
                    }
                }
                else if (bytesRead == 0)
                {
                    connection_closed = true;
                    break;
                }
                else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break; 
                    }
                    perror("read error");
                    connection_closed = true;
                    break;
                }
            }

            if (connection_closed)
            {
                // std::cout << "[FD " << client_fd << "] Disconnected. Cleaning up heap memory.\n";
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                close(client_fd);
                delete session; 
            }
            else
            {
                if (request_complete)
                {
                    HTTPRequest req{};
                    // Parse using the persistent session buffer
                    HTTPServer::parseHttpRequest(session->incomplete_buffer, &req);

                    if (req.method == "GET")
                    {
                        HTTPResponse h = HTTPResponse("GET", "200", "OK");
                        h.add_content_plain_text("\nHELLO GAMERS\n");
                        std::string t = h.get_response();

                        sendAll(client_fd, t);
                    }

                    // We advertised Connection: close, so close now. Closing first puts
                    // TIME_WAIT on the server side instead of exhausting client ports
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                    delete session;
                    continue;
                }

                // 6. Re-arm the socket safely using EPOLLONESHOT
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                ev.data.ptr = session; 

                if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev) == -1) {
                    perror("epoll_ctl MOD failed");
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                    delete session;
                }
            }
        }
    }
}


    void spawnClientFdThread(int clientFd)
    {
        // What this does:
        // If poll returns: > 0 Data is ready to read
        // 0 : timeout has reached (nothing received)
        // < 0 : an error has occurred
        //
        // std::cout << "Client thread spawned with clientFd => " << clientFd << std::endl;
        struct pollfd pfd;
        pfd.fd = clientFd;
        pfd.events = POLLIN; // Only care about incoming data

        bool request_complete = false;
        std::string full_req;

        while (running)
        {
            // Remove this as poll will sleep automatically
            // this_thread::sleep_for(defaultSleepTime);

            int pollResult = poll(&pfd, 1, 250);

            if (pollResult > 0)
            {
                if (pfd.revents & POLLIN)
                {
                    // 1. Data is waiting! Allocate a buffer and read it
                    // Read until you hit Termination Signal By HTTP!
                    char buffer[4096] = {0};

                    // Return bytes read
                    // printf("Beginning Poll Recv\n");
                    ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer), 0);
                    if (bytesRead > 0)
                    {
                        // Successfully received data -> process it here
                        full_req.append(buffer, bytesRead);
                        if (full_req.find("\r\n\r\n") != std::string::npos)
                        {
                            request_complete = true;
                            break; // Exits the while (running) loop
                        }
                    }
                    else if (bytesRead == 0)
                    {
                        // Client disconnected gracefully
                        // std::cout << "Client disconnected.\n";
                        break;
                    }
                    else
                    {
                        // Socket error occurred
                        break;
                    }
                }
            }
            else if (pollResult < 0)
            {
                // Poll system call error
                break;
            }
        } // End of while (running)

        // LOGIC FIX: Moved outside the while-loop so it executes after hitting 'break'
        if (request_complete)
        {
            // std::cout << "READ CURL REQ: => " << full_req << std::endl;

            // Parse Req Here
            HTTPRequest req{};
            HTTPServer::parseHttpRequest(full_req, &req);

            // Send CurlReqBack
            //
            if (req.method == "GET")
            {
                HTTPResponse h = HTTPResponse("GET", "200", "OK" );
                h.add_content_plain_text("\nHELLO GAMERS\n");
                std::string t = h.get_response();
                // printf("Sending response:\n %s", t.data());

                sendAll(clientFd, t);
            }
        }

        // Clean up socket when done
        close(clientFd);
    }

    // optimization: do const std::string& req (avoid copying values)
    // Takes 2 params req and requestBuild
    // req, string containing a full HTTP request (excluding Body)
    // requestBuild variable to fill in vars with
    void parseHttpRequest(std::string req, HTTPRequest* requestBuild)
    {

        std::istringstream stream(req);
        std::string line;

        // First Get first 3 args (method endpoint, http/versionnum)
        if(std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
            }

            std::istringstream line_stream(line);

            if (line_stream >> requestBuild->method >> requestBuild->target >> requestBuild->version)
            {
                // std::cout << "parsed request with method: " <<
                //     requestBuild->method <<
                //     "\nTarget: "<< requestBuild->target <<
                //     "\nVersion: " << requestBuild->version <<
                //     std::endl;
            } else {
            }

        }
        // Now get next headers
        while (std::getline(stream, line))
        {
            // Remove trailing carriage return
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Empty line = end of headers
            if (line.empty())
                break;

            size_t colon = line.find(':');

            if (colon == std::string::npos) {
                return;
            }

            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);

            // Remove leading whitespace from value
            size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos)
                value.erase(0, start);
            else
                value.clear();

            requestBuild->headers[key] = value;
        }


    }

    // periodically checks if threads need to be joined
    void garbageCollector()
    {
        auto it = clientThreads.begin();

        while (it != clientThreads.end())
        {
            if ((*it)->finished.load())
            {
                if ((*it)->thrd.joinable()) {
                    (*it)->thrd.join();
                }

                it = clientThreads.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    // Guarantee Send all bytes
    bool sendAll(int clientFd, const std::string& response)
    {
        size_t totalSent = 0;
        size_t totalSize = response.size();

        while(totalSent < totalSize)
        {
            ssize_t bytesSent = send(
                clientFd,
                response.data()+ totalSent,
                totalSize - totalSent,
                MSG_NOSIGNAL
            );

            if (bytesSent > 0)
            {
                totalSent += static_cast<size_t>(bytesSent);
            }
            else if(bytesSent == -1 && errno == EINTR)
                continue;
            else
            {
                perror("send failed");
                return false;
            }
        }
        return true;
    }

};



volatile std::sig_atomic_t stopRequested = 0;

void handleSigint(int) {
    stopRequested = 1;
}

int main()
{
    std::signal(SIGINT, handleSigint);

    HTTPServer server;

    while (!stopRequested) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
    }

    return 0; // Destructor called automatically
}
