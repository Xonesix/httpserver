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
#define PORT 8081 // Defined placeholder for completeness
#define PROTOCOL "HTTP/1.1"

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

        std::cout << "Server bound to port " << PORT << '\n';

        running.store(true);

        listenerThread = std::thread(
            &HTTPServer::spawnListeningThread, this
        );
    }
    ~HTTPServer()
    {
        running.store(false);

        if (listenerThread.joinable()) {
            listenerThread.join();
        }

        for (auto& worker : clientThreads) {
            if (worker->thrd.joinable()) {
                worker->thrd.join();
            }
        }

        close(serverSocket);
    }

private:
    void spawnListeningThread()
    {
        std::cout << "Listening thread spawned" << std::endl;
        pollfd pfd{};
        pfd.fd = serverSocket;
        pfd.events = POLLIN;


        // Once we get one, we accept and store into a thread map
        while (running.load())
        {
            garbageCollector();

            int result = poll(&pfd, 1, 250); // poll until 250ms passes

            if (result < 0)
            {
                if (errno == EINTR) continue;
                break;
            }

            if (result == 0) continue;

            if (pfd.revents & POLLIN)
            {
                int tempClientFd = accept(serverSocket, nullptr, nullptr);

                if (tempClientFd < 0) {
                    perror("accept");
                    continue;
                }

                auto worker = std::make_unique<Worker>();
                Worker* ptr = worker.get();

                worker->thrd = std::thread([this, tempClientFd, ptr](){
                    spawnClientFdThread(tempClientFd);

                    // finish the thread
                    ptr->finished.store(true);
                });
                clientThreads.push_back(std::move(worker));

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
                        std::cerr << "Read error occurred.\n";
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

                if(!sendAll(clientFd, t))
                {
                    std::cout << "failed send" << std::endl;
                }
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
                std::cout << "Failed parsing req line" << std::endl;
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
                std::cerr << "Invalid header\n";
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
