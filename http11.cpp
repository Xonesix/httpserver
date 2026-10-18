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
#define PORT 8081 // Defined placeholder for completeness

struct HTTPRequest {
     std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HTTPResponse 
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
    std::vector<std::thread> clientThreads{};
public:
    HTTPServer()
    {
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        serverAddress = {};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(PORT);
        serverAddress.sin_addr.s_addr = INADDR_ANY;

        if (bind(serverSocket,
                 reinterpret_cast<sockaddr*>(&serverAddress),
                 sizeof(serverAddress)) < 0) {
            throw std::runtime_error("bind failed");
        }

        if (listen(serverSocket, 5) < 0) {
            throw std::runtime_error("listen failed");
        }

        std::cout << "Server Bound to port: " << PORT << std::endl;

        running.store(true);

        listenerThread = std::thread(
            &HTTPServer::spawnListeningThread, this
        );


    }

    ~HTTPServer()
    {
        running.store(false);

        // Stop listener before accessing clientThreads.
        if (listenerThread.joinable()) {
            listenerThread.join();
        }

        for (auto& thread : clientThreads) {
            if (thread.joinable()) {
                thread.join();
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

                clientThreads.emplace_back(
                    &HTTPServer::spawnClientFdThread,
                    this,
                    tempClientFd
                );
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
        std::cout << "Client thread spawned with clientFd => " << clientFd << std::endl;
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
                    printf("Beginning Poll Recv\n");
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
                        std::cout << "Client disconnected.\n";
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
            std::cout << "READ CURL REQ: => " << full_req << std::endl;

            // Parse Req Here
            HTTPRequest req{};
            HTTPServer::parseHttpRequest(full_req, &req);

            // Send CurlReqBack
            // 
            if (req.method == "GET")
            {
                
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
                std::cout << "parsed request with method: " <<
                    requestBuild->method <<
                    "\nTarget: "<< requestBuild->target <<
                    "\nVersion: " << requestBuild->version <<
                    std::endl;
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
