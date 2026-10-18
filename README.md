# HTTP Server

An HTTP/1.1 server written from scratch in C++17 on raw POSIX sockets (`socket` / `bind` / `listen` / `poll` / `accept`), with its own request parser and response builder. Improved version by version, with each change measured.

## Performance

| Version | Change | Requests/sec | p99 latency | Max latency | Errors |
|---|---|---:|---:|---:|---:|
| V1 | Thread per client | 1,053 | 9.49 ms | 1,018 ms | 1.02% (crashed) |
| V2 | Thread cleanup, no hot-path logging, backlog 5 → 128 | **5,154** | 8.18 ms | 17 ms | **0%** |

**V1 → V2: ~4.9× throughput, ~59× lower max latency, zero errors, no more crashes.**

<sub>Python load generator (`load_testing.py`), 28 concurrent clients, 30 s, new TCP connection per request.</sub>

The Python client hits its own limit at about 5k RPS, so V2 was also benchmarked with **wrk**:

| Version | Connections | Requests/sec | p50 | p99 | Errors |
|---|---:|---:|---:|---:|---:|
| V2 | 28 | **62,607** | 0.35 ms | 0.65 ms | 0 |
| V2 | 100 | 53,172 | 1.83 ms | 2.51 ms | 0 |
| V2 | 200 | 58,772 | 2.02 ms | 122 ms | 20 timeouts |

<sub>`wrk -t8 -c<N> -d30s --latency -H 'Connection: close'`, localhost (WSL2).</sub>

Full results and analysis: [`bench-results.md`](bench-results.md)

## Iterations

**V1: Thread per client.** A listener thread accepts connections and starts one `std::thread` per client to parse the request and respond. Threads were never joined, so they piled up until the server ran out of resources and crashed.

**V2: Thread cleanup.** Each worker marks itself finished and a garbage collector joins finished threads. Also: per-request logging removed, listen backlog raised from 5 to 128, `sendAll()` with `MSG_NOSIGNAL`, `SO_REUSEADDR`, and an `-O3 -flto` build.

**V3 (next):** fixed-size thread pool, HTTP/1.1 keep-alive, then an `epoll` event loop.

## Build & run

```sh
g++ -std=c++17 -O3 -march=native -flto -DNDEBUG -pthread http11.cpp -o httpserver
./httpserver            # listens on :8081
curl http://127.0.0.1:8081/
```
