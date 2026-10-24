# HTTP/1.1 Server — Benchmark Results

## Version 1 — Thread-per-connection, threads never joined

**Date:** 2026-10-18 · **Commit:** `e25549b` ("HTTP1.1 done") · **Source:** `http11.cpp`

### Architecture

- One listener thread that `poll()`s the listening socket (250 ms timeout) and `accept()`s new connections.
- Every accepted connection gets a new `std::thread` (`spawnClientFdThread`) that reads until `\r\n\r\n`, parses the request, sends a fixed `HELLO GAMERS` `text/plain` response with `Connection: close`, and closes the socket.
- Each `std::thread` object is pushed into a `std::vector` (`clientThreads`) and only joined in the server destructor at shutdown.
- `listen()` backlog is **5**.
- Verbose logging (`std::cout` / `printf`) of every request and response on the hot path.

### Test setup

| Setting     | Value                                                       |
|-------------|-------------------------------------------------------------|
| Tool        | `load_testing.py` (Python stdlib, thread pool load generator) |
| Target      | `GET http://127.0.0.1:8080/`                                |
| Clients     | 28 concurrent workers                                       |
| Duration    | 30 s                                                        |
| Connections | New TCP connection per request (`Connection: close`)        |
| Host        | Linux 6.6 (WSL2), client and server on the same machine     |

### Results

| Metric         | Value                                          |
|----------------|------------------------------------------------|
| Elapsed        | 31.10 s (includes in-flight requests finishing) |
| Attempted      | 33,073                                         |
| Successful     | 32,737                                         |
| Failed         | 336 (1.02%)                                    |
| **Successful RPS** | **1,052.6**                                |

| Latency | Value      |
|---------|------------|
| mean    | 5.53 ms    |
| p50     | 5.36 ms    |
| p90     | 7.44 ms    |
| p95     | 8.11 ms    |
| p99     | 9.49 ms    |
| p99.9   | 11.18 ms   |
| max     | 1018.40 ms |

**Errors (top 10):**

```
336  TimeoutError: timed out
```

> Latency includes TCP connect + full HTTP response; the Python client can be a bottleneck itself.

### Summary

~1,000 RPS with a 1% error rate, and the server crashes under sustained load because client threads are never joined.

### What went wrong

1. **Threads were never joined (the main problem / the crash).**
   Each connection creates a `std::thread` and stores it in `clientThreads`, but nothing joins it until the server shuts down. When a handler returns, its thread has finished but is never reaped, so the thread's stack and kernel resources stay allocated. Over a 30 s run that is 33k+ threads that pile up. Memory and the per-process thread limit eventually run out, `std::thread` construction throws `std::system_error` ("Resource temporarily unavailable"), and since nothing catches it inside the listener thread, `std::terminate()` kills the process. The vector also keeps growing without limit.

2. **Creating a thread for every request is expensive.**
   Spawning and tearing down an OS thread per connection costs far more than the work being done (sending a 14-byte body). That overhead, plus a new TCP handshake per request, holds throughput to ~1k RPS.

3. **A listen backlog of 5 causes timeouts and the 1 s tail.**
   With 28 clients connecting nonstop, the accept queue of 5 fills up whenever the single listener thread falls behind (it is busy creating threads). The kernel then drops incoming SYNs, and the client retransmits after about 1 s. That fits the 1018 ms max latency, which is one SYN retry, while the p99.9 is only 11 ms. Connections that miss again end up as the 336 `TimeoutError`s.

4. **Synchronous logging on the hot path.**
   Every request prints the full raw request, the parsed fields, and the full response to stdout. `std::cout` and `printf` are slow, take locks, and make all handler threads contend with each other.

5. **Smaller inefficiencies.**
   - `parseHttpRequest` takes the request `std::string` by value (an extra copy) and parses with `istringstream`.
   - The response is built with an `ostringstream` every time, even though the response never changes.
   - `poll()` with a 250 ms timeout in each client thread adds wakeups that do nothing.

### Plan for Version 2

- **Fix the leak:** `detach()` handler threads, or have finished workers flag themselves (the `Worker { std::thread; std::atomic<bool> finished; }` idea) and let a garbage-collector thread join them regularly.
- **Better:** replace thread-per-connection with a fixed-size thread pool and a work queue, so thread count stays bounded no matter the load.
- Raise the `listen()` backlog (e.g. `SOMAXCONN`) to get rid of SYN drops and the 1 s tail latency.
- Remove or gate the per-request logging.
- **Later:** HTTP/1.1 keep-alive (reuse connections), then non-blocking I/O with `epoll` to serve many connections per thread.
- Track memory usage and thread count during runs (see `benchmark.md`) to show the leak is gone.

---

## Version 2 — Thread-per-connection + thread cleanup, debug logging removed

**Date:** 2026-10-18 · **Commit:** uncommitted changes on top of `e25549b` · **Source:** `http11.cpp` · **Port:** 8081

### Build

From `build.txt`:

```sh
g++ -std=c++17 -O3 -march=native -flto -DNDEBUG -pthread http11.cpp -o httpserver
```

- `-O3`: full optimization.
- `-march=native`: generate code for the CPU it is built on.
- `-flto`: link-time optimization.
- `-DNDEBUG`: turns off `assert()`s.
- `-pthread`: thread support.

Version 1 was run from `a.out`, built without a recorded command or optimization flags. Some of the speedup may come from the optimized build.

### What changed

1. **Thread cleanup (fixes the crash).**
   Each connection's thread is now wrapped in a `Worker { std::thread thrd; std::atomic<bool> finished; }`, stored as `std::unique_ptr<Worker>`. The thread runs the handler and then sets `finished = true`. Every time the listener loop runs (after each accept, and at least every 250 ms), `garbageCollector()` goes through `clientThreads`, joins every finished worker, and erases it. The number of live threads now stays close to the number of connections being served, not the total number of requests since startup, so memory stays flat and the crash is gone.

2. **Debug logging removed from the hot path.**
   The per-request `std::cout` / `printf` calls (raw request dump, parsed fields, response dump, "thread spawned", "Beginning Poll Recv", "Client disconnected") are commented out. Threads no longer fight over the stdout lock or wait on terminal I/O.

3. **`listen()` backlog raised from 5 to 128.**
   The accept queue no longer overflows under 28 constantly reconnecting clients, so the kernel stops dropping SYNs. This removes the ~1 s SYN-retry tail (max latency 1018 ms → 17 ms) and all the timeouts.

4. **`sendAll()` with `MSG_NOSIGNAL`.**
   The response is sent in a loop until every byte is written (handling partial sends and `EINTR`). `MSG_NOSIGNAL` stops a client disconnecting mid-response from raising `SIGPIPE` and killing the server.

5. **`SO_REUSEADDR` and error handling on setup.**
   The server can restart right away without `bind failed` while old sockets sit in `TIME_WAIT`. `socket` / `bind` / `listen` failures now `perror` and close the socket.

### Test setup

Same as Version 1: `load_testing.py`, `GET http://127.0.0.1:8081/`, 28 clients, 30 s, new TCP connection per request, WSL2 on the same machine.

```sh
python3 load_testing.py --host 127.0.0.1 --port 8081 -c 28 -d 30
```

### Results

| Metric             | Value       |
|--------------------|-------------|
| Elapsed            | 30.00 s     |
| Attempted          | 154,627     |
| Successful         | 154,627     |
| Failed             | 0 (0.00%)   |
| **Successful RPS** | **5,153.6** |

| Latency | Value    |
|---------|----------|
| mean    | 4.98 ms  |
| p50     | 4.89 ms  |
| p90     | 6.52 ms  |
| p95     | 7.05 ms  |
| p99     | 8.18 ms  |
| p99.9   | 9.60 ms  |
| max     | 17.36 ms |

**Errors:** none.

### Version 1 → Version 2

| Metric         | V1         | V2        | Change            |
|----------------|------------|-----------|-------------------|
| Successful RPS | 1,052.6    | 5,153.6   | **~4.9× faster**  |
| Requests in 30 s | 32,737   | 154,627   | ~4.7× more        |
| Error rate     | 1.02%      | 0.00%     | timeouts gone     |
| p50 latency    | 5.36 ms    | 4.89 ms   | −9%               |
| p99 latency    | 9.49 ms    | 8.18 ms   | −14%              |
| p99.9 latency  | 11.18 ms   | 9.60 ms   | −14%              |
| Max latency    | 1018.40 ms | 17.36 ms  | **~59× lower**    |
| Stability      | crashed under sustained load | stable for the full run | thread leak fixed |

**Resume line:** cut tail latency 59× and raised throughput ~5× (1.05k → 5.15k RPS) with zero errors by fixing a thread leak, raising the accept backlog, and removing hot-path logging.

### Why RPS went up so much while p50 barely moved

Latency is measured per request from the client and includes the TCP handshake, so p50 only dropped slightly. Throughput went up mainly because in V1 workers spent time stuck behind the full backlog (some waiting ~1 s for SYN retries or up to the timeout) and handlers were serialized on stdout logging. With both gone, the 28 clients keep their requests flowing. The p50 sits close to what one connect + request + close costs on WSL2 with a Python client, so **the load generator is now probably the bottleneck**. The next round should use `wrk` or `oha` (or more client processes) to find the server's real limit.

### Concurrency sweep (same V2 build, no code changes)

`-c` sets how many client workers run at once. Each worker keeps one request in flight at a time.

```sh
python3 load_testing.py --host 127.0.0.1 --port 8081 -c <28|100|200> -d 30
```

| Clients | Requests (30 s) | Errors | RPS     | mean     | p50      | p95      | p99      | p99.9    | max       |
|--------:|----------------:|-------:|--------:|---------:|---------:|---------:|---------:|---------:|----------:|
| 28      | 154,627         | 0      | 5,153.6 | 4.98 ms  | 4.89 ms  | 7.05 ms  | 8.18 ms  | 9.60 ms  | 17.36 ms  |
| 100     | 154,934         | 0      | 5,162.4 | 17.83 ms | 17.60 ms | 25.38 ms | 28.93 ms | 34.80 ms | 154.27 ms |
| 200     | 151,747         | 0      | 5,053.7 | 36.24 ms | 35.77 ms | 51.55 ms | 59.15 ms | 76.13 ms | 456.81 ms |

**What this shows**

- **Throughput is flat at ~5.1k RPS.** Going from 28 to 200 clients (7×) adds no throughput. The system was already saturated at 28 clients.
- **Latency grows in line with the client count.** This is Little's Law: *concurrency = throughput × latency*, so latency ≈ clients / RPS:
  - 28 / 5,154 ≈ 5.4 ms (measured mean 5.0 ms)
  - 100 / 5,162 ≈ 19.4 ms (measured 17.8 ms)
  - 200 / 5,054 ≈ 39.6 ms (measured 36.2 ms)

  Extra clients don't get served faster. They just wait in line for the same fixed-rate pipeline.
- **Still 0 errors at 200 clients.** With the backlog at 128 and the thread leak fixed, the server stays correct and stable under 7× more concurrency. It only queues more. In V1 this load would have crashed it.
- **The worst case grows** (17 ms → 154 ms → 457 ms) because of queueing in the accept backlog and scheduling across ~200 threads on the client and server. It's still well below V1's 1 s SYN-retry stalls.

**Where the ~5.1k RPS ceiling probably comes from**

Two suspects:

1. **The load generator (most likely).** `load_testing.py` is a single Python process using threads, so the GIL limits it to roughly one core of Python work. Opening a socket, sending, parsing the response, and closing all happen in Python, and more threads don't add capacity.
2. **The server's single accept thread.** One listener thread runs `accept()` + `std::thread` creation + `garbageCollector()` for every connection, which also handles connections one at a time.

**Answer: it was the load generator.** The wrk runs below push the same server to ~62k RPS.

### wrk benchmark (same V2 build, no code changes)

`load_testing.py` couldn't push the server any harder, so the same three loads were rerun with **wrk 4.1.0** (Debian package), a C load generator built on `epoll`.

```sh
for c in 28 100 200; do
  wrk -t8 -c$c -d30s --latency -H 'Connection: close' http://127.0.0.1:8081/
done
```

- `-t8`: 8 client threads, so the client isn't stuck on one core.
- `-c`: number of open connections (same meaning as `-c` in the Python script).
- `-H 'Connection: close'`: each request uses a new TCP connection, same as the Python tests. The server closes the connection after every response anyway.
- `--latency`: print the latency percentiles.

| Connections | Requests (30 s) | **RPS**    | Transfer   | Avg     | Stdev    | p50     | p75     | p90     | p99       | Max     | Errors      |
|------------:|----------------:|-----------:|-----------:|--------:|---------:|--------:|--------:|--------:|----------:|--------:|-------------|
| 28          | 1,884,437       | **62,607** | 5.85 MB/s  | 0.37 ms | 0.07 ms  | 0.35 ms | 0.38 ms | 0.43 ms | 0.65 ms   | 4.85 ms | 0           |
| 100         | 1,600,441       | **53,172** | 4.97 MB/s  | 1.78 ms | 0.30 ms  | 1.83 ms | 1.98 ms | 2.13 ms | 2.51 ms   | 6.34 ms | 0           |
| 200         | 1,769,012       | **58,772** | 5.49 MB/s  | 7.68 ms | 68.73 ms | 2.02 ms | 2.14 ms | 2.32 ms | 122.06 ms | 1.67 s  | 20 timeouts |

The server stayed up for all three runs (same process, same PID, about 5.25M requests in total).

**Python client vs wrk (same server)**

| Connections | `load_testing.py` RPS | wrk RPS | wrk / Python |
|------------:|----------------------:|--------:|-------------:|
| 28          | 5,154                 | 62,607  | **~12.1×**   |
| 100         | 5,162                 | 53,172  | ~10.3×       |
| 200         | 5,054                 | 58,772  | ~11.6×       |

**What this shows**

- **The real V2 throughput is ~53–63k RPS**, not ~5k. The Python tester was hitting its own limit (the GIL) and hiding about 90% of what the server can do.
- **Latency isn't directly comparable across the two tools.** `load_testing.py` times the TCP connect plus the response. wrk times from writing the request to reading the response, and opens the next connection outside that timer. wrk's numbers are closer to the server's own processing time, and Python's to what an end user feels.
- **The server is saturated now.** Going from 28 to 100 connections drops RPS (62.6k → 53.2k) and p50 jumps (0.35 → 1.83 ms). More connections means more threads alive at once, more context switching, and more work for the single accept thread. RPS no longer grows with load.
- **At 200 connections the queue starts to overflow.** p50 stays at about 2 ms, but p99 jumps to 122 ms, max to 1.67 s, and 20 requests time out (wrk's default 2 s timeout). With 200 connections constantly reconnecting and a backlog of 128, the accept queue sometimes overflows and SYNs get dropped. This is the same failure as V1's 1 s retries, now happening at about 60× the load.
- **The limit is now the server's design.** One thread runs `accept()` + `std::thread` creation + `garbageCollector()` for every request, and every request pays for a TCP handshake and the creation and teardown of an OS thread. At ~60k RPS that's ~60k threads created per second.

**Rerun (headline number, commit `e00c216`):**

```sh
wrk -t4 -c28 -d30s --latency http://127.0.0.1:8081/
```

| Connections | Requests (30 s) | **RPS**    | Transfer  | Avg     | Stdev   | p50      | p75      | p90      | p99      | Max     | Errors |
|------------:|----------------:|-----------:|----------:|--------:|--------:|---------:|---------:|---------:|---------:|--------:|--------|
| 28          | 1,859,424       | **61,777** | 5.77 MB/s | 0.43 ms | 0.08 ms | 0.416 ms | 0.449 ms | 0.506 ms | 0.746 ms | 4.26 ms | 0      |

This run used 4 wrk threads and no `Connection: close` header. The server still closes after every response, so each request is still a new TCP connection. It lands within about 1.3% of the `-t8` run (62,607 RPS), so the result is repeatable.

**Resume line (wrk):** benchmarked **61.8K requests/sec** across 1.86M HTTP requests using wrk, at **0.416 ms p50 / 0.746 ms p99** with 28 concurrent localhost connections and zero errors.

### What's still not great

- **Still one OS thread per connection.** Creating and destroying a thread for each request is still the most expensive thing the server does. Under a big burst of connections the thread count is unbounded.
- **Cleanup only runs on the listener loop.** Finished threads are only reaped when the listener wakes up (after each accept, or every 250 ms). `vector::erase` in the middle is O(n), which is fine at this scale but adds up with thousands of live workers.
- **No keep-alive.** Every request pays for a full TCP handshake and teardown (`Connection: close`).
- **Response rebuilt every request** with `ostringstream`; parsing still copies the request string and uses `istringstream`.
- **Memory and thread count weren't measured** during the run (see `benchmark.md`). Recording peak RSS and thread count would show the leak fix with numbers.

### Plan for Version 3

- Replace thread-per-connection with a **fixed-size thread pool + work queue** (e.g. one thread per core) so the thread count is bounded and threads aren't created per request.
- Add **HTTP/1.1 keep-alive** so connections are reused.
- Pre-build the static response once; take the request by `const std::string&` / `std::string_view`.
- Raise the `listen()` backlog to `SOMAXCONN` (and check `net.core.somaxconn`) to remove the SYN-drop timeouts at 200+ connections.
- Use **wrk as the main benchmark** from now on (`load_testing.py` tops out at ~5k RPS). Add a keep-alive run (no `Connection: close` header) once keep-alive exists, and record memory and thread count.
- Later: non-blocking sockets + `epoll` event loop.
