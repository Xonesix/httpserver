#!/usr/bin/env python3
"""Simple HTTP/1.1 GET load generator (one TCP connection per request).

Standard library only. Intended for benchmarking servers you own/control.
"""
import argparse
import concurrent.futures
import socket
import statistics
import threading
import time
from collections import Counter


def percentile(sorted_values, p):
    if not sorted_values:
        return float('nan')
    rank = (len(sorted_values) - 1) * p / 100
    lo = int(rank)
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (rank - lo)


def one_request(host, port, path, timeout, expected_status):
    start = time.perf_counter_ns()
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            request = (f"GET {path} HTTP/1.1\r\n"
                       f"Host: {host}:{port}\r\n"
                       "Connection: close\r\n"
                       "User-Agent: python-http-bench/1.0\r\n"
                       "\r\n").encode('ascii')
            sock.sendall(request)
            response = bytearray()
            header_end = -1
            expected_total = None
            status = None
            while True:
                chunk = sock.recv(16384)
                if not chunk:
                    break
                response.extend(chunk)
                if header_end == -1:
                    header_end = response.find(b'\r\n\r\n')
                    if header_end >= 0:
                        raw_headers = response[:header_end].split(b'\r\n')
                        parts = raw_headers[0].split(b' ', 2)
                        if len(parts) < 2 or not parts[0].startswith(b'HTTP/'):
                            raise ValueError('invalid HTTP status line')
                        status = int(parts[1])
                        content_lengths = [v.strip() for h in raw_headers[1:] if b':' in h for k,v in [h.split(b':', 1)] if k.lower() == b'content-length']
                        if len(content_lengths) == 1:
                            length = int(content_lengths[0])
                            if length < 0:
                                raise ValueError('negative Content-Length')
                            expected_total = header_end + 4 + length
                        elif len(content_lengths) > 1:
                            raise ValueError('duplicate Content-Length')
                if expected_total is not None and len(response) >= expected_total:
                    break
            if header_end == -1:
                raise ValueError('missing HTTP headers')
            if expected_total is not None and len(response) != expected_total:
                raise ValueError('body length mismatch')
            if status != expected_status:
                raise ValueError(f'HTTP {status} (expected {expected_status})')
            ms = (time.perf_counter_ns() - start) / 1_000_000
            return ms, None
    except (OSError, ValueError, UnicodeError) as exc:
        return None, f'{type(exc).__name__}: {exc}'


def worker(start_barrier, stop_at, host, port, path, timeout, expected_status):
    latencies = []
    errors = Counter()
    attempts = 0
    start_barrier.wait()
    while time.perf_counter() < stop_at[0]:
        attempts += 1
        latency, error = one_request(host, port, path, timeout, expected_status)
        if error is None:
            latencies.append(latency)
        else:
            errors[error] += 1
    return attempts, latencies, errors


def main():
    parser = argparse.ArgumentParser(description='GET benchmark, new connection per request, standard library only')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--path', default='/')
    parser.add_argument('-c', '--clients', type=int, default=28, help='concurrent client worker threads (default: 28)')
    parser.add_argument('-d', '--duration', type=float, default=15, help='seconds of load (default: 15)')
    parser.add_argument('--timeout', type=float, default=2, help='per-operation socket timeout seconds (default: 2)')
    parser.add_argument('--status', type=int, default=200, help='expected HTTP status (default: 200)')
    args = parser.parse_args()
    if args.clients < 1 or args.duration <= 0 or args.timeout <= 0 or not args.path.startswith('/'):
        parser.error('clients, duration, timeout must be positive; path must start with /')
    if not (1 <= args.port <= 65535):
        parser.error('port must be between 1 and 65535')

    print(f'GET http://{args.host}:{args.port}{args.path} | {args.clients} clients | {args.duration:g}s | new TCP connection/request', flush=True)
    barrier = threading.Barrier(args.clients + 1)
    stop_at = [0.0]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as executor:
        futures = [executor.submit(worker, barrier, stop_at, args.host, args.port, args.path, args.timeout, args.status)
                   for _ in range(args.clients)]
        start = time.perf_counter()
        stop_at[0] = start + args.duration
        barrier.wait()
        results = [future.result() for future in futures]
    elapsed = time.perf_counter() - start
    attempts = sum(result[0] for result in results)
    latencies = sorted(latency for result in results for latency in result[1])
    errors = Counter()
    for _, _, worker_errors in results:
        errors.update(worker_errors)
    success = len(latencies)
    print(f'Elapsed:          {elapsed:.2f} s (includes in-flight requests finishing)')
    print(f'Attempted:        {attempts:,}')
    print(f'Successful:       {success:,}')
    print(f'Failed:           {attempts - success:,} ({100 * (attempts-success) / attempts if attempts else 0:.2f}%)')
    print(f'Successful RPS:   {success / elapsed:,.1f}')
    if latencies:
        print(f'Latency mean:    {statistics.mean(latencies):.2f} ms')
        for p in (50, 90, 95, 99, 99.9):
            print(f'Latency p{p:g}:     {percentile(latencies, p):.2f} ms')
        print(f'Latency max:     {latencies[-1]:.2f} ms')
    if errors:
        print('Errors (top 10):')
        for error, count in errors.most_common(10):
            print(f'  {count:>7}  {error}')
    print('Note: latency includes TCP connect + full HTTP response; Python client can bottleneck.')


if __name__ == '__main__':
    main()
