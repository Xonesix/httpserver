# HTTP Server

An HTTP/1.1 server written from scratch in C++17 on raw POSIX sockets (`socket` / `bind` / `listen` / `accept` / `epoll`), with its own request parser and response builder. Improved version by version, with each change measured. It went from a thread-per-client design that crashed at ~1k requests/sec to a non-blocking `epoll` event loop that serves **~410k requests/sec**.

## Performance

| Version | Change | Requests/sec | p99 latency | Max latency | Errors |
|---|---|---:|---:|---:|---:|
| V1 | Thread per client | 1,053 | 9.49 ms | 1,018 ms | 1.02% (crashed) |
| V2 | Thread cleanup, no hot-path logging, backlog 5 → 128 | **5,154** | 8.18 ms | 17 ms | **0%** |

**V1 → V2: ~4.9× throughput, ~59× lower max latency, zero errors, no more crashes.**

<sub>Python load generator (`load_testing.py`), 28 concurrent clients, 30 s, new TCP connection per request.</sub>

The Python client hits its own limit at about 5k RPS, so from V2 on the server is benchmarked with **wrk**. V2 and V3 below were run in the same session:

| Version | wrk load | Requests (30 s) | Requests/sec | p50 | p99 | Errors |
|---|---|---:|---:|---:|---:|---:|
| V2 thread-per-connection | 4 threads, 28 conns | 1.52M | 50,410 | 529 µs | 730 µs | 0 |
| **V3 epoll, 3 workers** | 4 threads, 28 conns | 6.33M | **210,273** | **54 µs** | **255 µs** | 0 |
| V2 thread-per-connection | 14 threads, 100 conns | 1.48M | 49,067 | 1.96 ms | 2.28 ms | 0 |
| **V3 epoll, 4 workers** | 14 threads, 100 conns | 12.33M | **409,623** | **142 µs** | **692 µs** | 0 |

**V2 → V3: 8.3× throughput (49k → 410k RPS), 13.8× lower p50 latency, zero errors, fixed thread count.**

<sub>`wrk -t<threads> -c<conns> -d30s --latency`, localhost (WSL2, 28 hardware threads). The server closes the connection after every response, so each request uses a new TCP connection.</sub>

Full results and analysis: [`bench-results.md`](bench-results.md)

## Iterations

**V1: Thread per client.** A listener thread accepts connections and starts one `std::thread` per client to parse the request and respond. Threads were never joined, so they piled up until the server ran out of resources and crashed.

**V2: Thread cleanup.** Each worker marks itself finished and a garbage collector joins finished threads. Also: per-request logging removed, listen backlog raised from 5 to 128, `sendAll()` with `MSG_NOSIGNAL`, `SO_REUSEADDR`, and an `-O3 -flto` build.

**V3: `epoll` event loop + fixed worker pool** ([`http11_epoll.cpp`](http11_epoll.cpp)). Creating an OS thread for every connection was the bottleneck, so V3 swaps it for non-blocking I/O:

- A **listener thread** accepts connections, makes each socket non-blocking, and registers it with a shared `epoll` instance (`EPOLLIN | EPOLLET | EPOLLONESHOT`).
- A **small, fixed pool of worker threads** (3–4) wait in `epoll_wait`. A worker only wakes up when a socket actually has data. It reads until `EAGAIN`, then parses, responds and closes.
- **Per-connection session buffers** put requests split across several TCP packets back together. **`EPOLLONESHOT`** guarantees that only one worker handles a socket at a time, so no locks are needed.
- The thread count stays constant no matter how many connections are open, and the process peaks at about **3.6 MB** of memory.

Problems found and fixed while profiling:
- **Segfault:** epoll's `data` field is a union, and worker threads were reading the listening socket's fd as a session pointer. Fixed by giving the listener its own epoll instance.
- **10× throughput ceiling caused by TCP `TIME_WAIT`:** the server waited for the client to close first, which used up the client's ~28k ephemeral ports. Closing on the server side first took throughput from 21k to 205k RPS.
- **Worker-count tuning:** a sweep showed 3–4 workers is optimal. At 8 or more, contention on the shared epoll instance makes it 3–6× slower.

**V4 (next):** one listener + epoll instance per thread using `SO_REUSEPORT`, so throughput keeps growing with the number of cores. Then HTTP/1.1 keep-alive.

## Build & run

```sh
# V3: epoll server
g++ -std=c++17 -O3 -march=native -flto -DNDEBUG -pthread http11_epoll.cpp -o httpserver_epoll
./httpserver_epoll      # listens on :8080
curl http://127.0.0.1:8080/

# V2: thread-per-connection server
g++ -std=c++17 -O3 -march=native -flto -DNDEBUG -pthread http11.cpp -o httpserver
./httpserver            # listens on :8081

# benchmark
wrk -t4 -c28 -d30s --latency http://127.0.0.1:8080/
```
