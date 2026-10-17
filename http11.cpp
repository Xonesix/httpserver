#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace std;

#define PORT 8081

// initially let's do a thread per connection
// 

class HTTPServer
{
    std::vector<thread> tStore = {};
    unordered_map<int, thread> fdToTreadMap = {};
  public: 
    HTTPServer()
    {
        // create the server socket
        // 
        int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in serverAddress;
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(PORT);
        serverAddress.sin_addr.s_addr = INADDR_ANY;

        // bind the socket to 
        int x = bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress));

        // we need a listening thread, once we get a connection, and spawn a thread for a connection
        
    }
  private:
      void spawnListeningThread()
      {
          
      }
      void spawnClientFdThread()
      {
          
      }
};


int main()
{
    // creating socket
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    // specifying the address
    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    // binding socket.
    bind(serverSocket, (struct sockaddr*)&serverAddress,
         sizeof(serverAddress));

    // listening to the assigned socket
    listen(serverSocket, 5);

    // accepting connection request
    int clientSocket
        = accept(serverSocket, nullptr, nullptr);

    // recieving data
    char buffer[1024] = { 0 };
    recv(clientSocket, buffer, sizeof(buffer), 0);
    cout << "Message from client: " << buffer << endl;

    //send msg back
    const char* msg = "Hello CLIENT I HAVE RECEIVED YOUR MESSAGE";
    send(clientSocket, msg, strlen(msg), 0);

    // closing the socket.
    close(serverSocket);

    return 0;
}