#include <iostream>
#include <sys/socket.h>

int main()
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    
    std::cout << "HI\n";
    return 0;
}