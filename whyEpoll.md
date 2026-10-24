In thread per client => each connection gets a thread that calls blocking read or write
simple because each thread's code reads top to bottom 

These threads spent most of the time asleep, waiting on sockets that has no data
Each thread costs
- memory
- context switches
- scheduler overhead
- creation cost


Ok, but what about only getting the ones that are ready!
With epoll
We register once `epoll_create1(0)`
`epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev)` - register a socket in interst list
- When data arrives on a socket, the callback puts this in the ready list
The kernel does this as packets come in

`epoll_wait` wil lhand us what's already on the ready List 



# 