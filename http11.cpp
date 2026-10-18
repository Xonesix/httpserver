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
#define PORT 8080 // Defined placeholder for completeness

class HTTPServer
{
private:
    std::unordered_map<int, std::thread> fdToThreadMap = {};
    int serverSocket;
    sockaddr_in serverAddress;
    std::atomic<bool> running;
    std::chrono::milliseconds defaultSleepTime = std::chrono::milliseconds(250);
    std::thread listenerThread;
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

        listenerThread = std::thread(
            &HTTPServer::spawnListeningThread, this
        );
    }

    ~HTTPServer()
    {
        running.store(false);

        // Listener must stop adding threads before we iterate the map.
        if (listenerThread.joinable()) {
            listenerThread.join();
        }

        // All workers must exit before the HTTPServer is destroyed.
        for (auto& [fd, thread] : fdToThreadMap) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        close(serverSocket);
    }

private:
    void spawnListeningThread()
    {
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
                if (tempClientFd < 0)
                {
                    perror("accept");
                    continue;
                }


                // place thread here
                fdToThreadMap.emplace(tempClientFd,
                    std::thread(&HTTPServer::spawnClientFdThread, this, tempClientFd));

            }
        }
    }

    void spawnClientFdThread(int clientFd)
    {
        // What this does:
        // If poll returns: > 0 Data is ready to read
        // 0 : timeout has reached (nothing received)
        // < 0 : an error has occurred
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
            // Send CurlReqBack
        }

        // Clean up socket when done
        close(clientFd);
    }
};



int main() {
    HTTPServer h;
    std::this_thread::sleep_for(std::chrono::minutes(5));
}
